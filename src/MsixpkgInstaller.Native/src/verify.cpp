#include "verify.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

#include "appx.h"

namespace msixpkg {
namespace {

void add_error(Report& report, const std::string& code, const std::string& message, const std::string& detail = {}) {
    report.errors.push_back(Issue{code, message, detail});
}

void add_error(Report& report, const Error& error) {
    report.errors.push_back(Issue{error.code(), error.message(), error.detail()});
}

void add_warning(Report& report, const std::string& code, const std::string& message, const std::string& detail = {}) {
    report.warnings.push_back(Issue{code, message, detail});
}

/// 解析最多四段数字版本；失败返回 false。
bool try_parse_version(std::string_view text, std::array<int, 4>& out) {
    out = {0, 0, 0, 0};
    size_t start = 0;
    int parts = 0;

    while (parts < 4) {
        const size_t dot = text.find('.', start);
        const std::string_view part = (dot == std::string_view::npos) ? text.substr(start) : text.substr(start, dot - start);
        if (part.empty() || part.size() > 9) {
            return parts > 0;  // 允许 "1.0" 这类省略尾部的写法
        }

        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return parts > 0;
            }
            value = value * 10 + (c - '0');
        }

        out[static_cast<size_t>(parts)] = value;
        ++parts;

        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    return parts > 0;
}

bool version_at_least(std::string_view actual, std::string_view required) {
    std::array<int, 4> a{};
    std::array<int, 4> r{};
    const bool parsed_actual = try_parse_version(actual, a);
    const bool parsed_required = try_parse_version(required, r);
    if (!parsed_actual || !parsed_required) {
        return true;  // 任一侧无法解析时不拦（避免因为格式怪而误拒）
    }
    return a >= r;
}

bool is_architecture_compatible(std::string_view package_architecture, std::string_view host_architecture) {
    if (package_architecture == host_architecture) {
        return true;
    }
    // x64 主机可以运行 x86 包。
    return host_architecture == "x64" && package_architecture == "x86";
}

bool thumbprint_pinned(const std::vector<std::string>& allowed, std::string_view thumbprint) {
    return std::any_of(allowed.begin(), allowed.end(), [thumbprint](const std::string& candidate) {
        return equals_ignore_case_ascii(candidate, thumbprint);
    });
}

void verify_declared_files(Container& container, Report& report) {
    const auto verify_one = [&container, &report](const FileRef& ref) {
        const zip::Entry* entry = container.archive().find(ref.path);
        if (entry == nullptr) {
            add_error(report, codes::FileMissing, "A declared payload entry is missing.", ref.path);
            return;
        }

        if (entry->uncompressed_size != ref.size) {
            add_error(report, codes::SizeMismatch, "A payload entry size does not match the manifest.",
                      ref.path + ": " + std::to_string(entry->uncompressed_size) + " != " + std::to_string(ref.size));
            return;
        }

        const std::string actual = container.entry_sha256(*entry);
        if (actual != ref.sha256) {
            add_error(report, codes::HashMismatch, "A payload entry hash does not match the manifest.",
                      ref.path + ": " + actual);
        }
    };

    verify_one(container.manifest().main_package);
    for (const FileRef& dependency : container.manifest().dependencies) {
        verify_one(dependency);
    }
}

void verify_publisher_certificate(Container& container, const ValidateOptions& options, Report& report) {
    const Manifest& manifest = container.manifest();
    if (!manifest.has_publisher) {
        if (options.require_pinned_publisher) {
            add_error(report, codes::NotPinned,
                      "No allowed publisher list can be satisfied because the container carries no certificate.");
        }
        return;
    }

    const zip::Entry* entry = container.archive().find(manifest.publisher.cert_path);
    if (entry == nullptr) {
        add_error(report, codes::CertificateMissing, "The publisher certificate entry is missing.",
                  manifest.publisher.cert_path);
        return;
    }

    Certificate certificate;
    try {
        const Bytes der = container.archive().read_to_memory(*entry, 4ull * 1024 * 1024);
        certificate = Certificate::from_der(der.data(), der.size());
    } catch (const Error& error) {
        add_error(report, error);
        return;
    }

    const std::string sha256 = certificate.sha256_thumbprint();
    const std::string sha1 = certificate.sha1_thumbprint();

    if (sha256 != manifest.publisher.thumbprint_sha256 || sha1 != manifest.publisher.thumbprint_sha1) {
        add_error(report, codes::CertificateMalformed,
                  "The publisher certificate thumbprint does not match the manifest.", sha256);
    }

    const std::string subject = certificate.subject_string();
    if (subject != manifest.publisher.subject) {
        add_error(report, codes::PublisherMismatch,
                  "The certificate subject does not match the manifest publisher entry.",
                  subject + " != " + manifest.publisher.subject);
    }

    if (subject != manifest.identity.publisher) {
        add_error(report, codes::PublisherMismatch,
                  "The certificate subject does not match the package identity publisher.",
                  subject + " != " + manifest.identity.publisher);
    }

    bool has_eku_extension = false;
    const bool code_signing = certificate.has_code_signing_eku(has_eku_extension);
    if (has_eku_extension && !code_signing) {
        add_error(report, codes::CertificateNotCodeSigning,
                  "The publisher certificate is not valid for code signing (missing EKU).",
                  "1.3.6.1.5.5.7.3.3");
    } else if (!has_eku_extension) {
        add_warning(report, codes::CertificateNotCodeSigning,
                    "The publisher certificate has no enhanced key usage extension; code signing usage cannot be confirmed.");
    }

    const uint64_t now = options.now_filetime != 0 ? options.now_filetime : now_filetime();
    if (now < certificate.not_before()) {
        add_error(report, codes::CertificateNotYetValid, "The publisher certificate is not yet valid.");
    } else if (now > certificate.not_after()) {
        add_error(report, codes::CertificateExpired, "The publisher certificate has expired.");
    } else {
        // 剩余不足 90 天（90 * 24 * 3600 * 10^7 个 100ns 单位）时告警。
        constexpr uint64_t kNinetyDays = 90ull * 24 * 3600 * 10000000ull;
        if (certificate.not_after() - now <= kNinetyDays) {
            add_warning(report, codes::CertificateExpired, "The publisher certificate expires within 90 days.");
        }
    }

    report.publisher_certificate = std::move(certificate);
    report.has_publisher_certificate = true;

    if (!options.allowed_publisher_thumbprints.empty()) {
        report.publisher_was_pinned = thumbprint_pinned(options.allowed_publisher_thumbprints, sha256);
        if (!report.publisher_was_pinned) {
            add_error(report, codes::NotPinned, "The publisher is not in the allowed publisher list.", sha256);
        }
    } else if (options.require_pinned_publisher) {
        add_error(report, codes::NotPinned, "No allowed publisher list is configured, but pinning is required.", sha256);
    }
}

void verify_container_signature(Container& container, const ValidateOptions& options, Report& report) {
    const zip::Entry* entry = container.archive().find("msixpkg.sig");
    if (entry == nullptr) {
        if (options.require_signature) {
            add_error(report, codes::SignatureMissing, "The container is not signed (msixpkg.sig is missing).");
        }
        // 即便不要求签名，也要算出内容摘要供上层记录。
        report.content_digest_hex = container.compute_content_digest();
        return;
    }

    Signature signature;
    try {
        const Bytes bytes = container.archive().read_to_memory(*entry, 1024ull * 1024);
        signature = Signature::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    } catch (const Error& error) {
        add_error(report, error);
        return;
    }

    const std::string digest = container.compute_content_digest();
    report.content_digest_hex = digest;

    if (digest != signature.content_digest) {
        add_error(report, codes::SignatureInvalid, "The container content digest does not match the signature.",
                  digest + " != " + signature.content_digest);
        return;
    }

    if (!report.has_publisher_certificate) {
        add_error(report, codes::CertificateMissing,
                  "The container signature cannot be verified without a publisher certificate.");
        return;
    }

    const std::string thumbprint = report.publisher_certificate.sha256_thumbprint();
    if (thumbprint != signature.key_thumbprint_sha256) {
        add_error(report, codes::SignatureThumbprintMismatch,
                  "The signature key thumbprint does not match the publisher certificate.",
                  signature.key_thumbprint_sha256 + " != " + thumbprint);
        return;
    }

    Bytes raw_signature;
    if (!base64_decode(signature.signature_base64, raw_signature)) {
        add_error(report, codes::SignatureMalformed, "The container signature is not valid base64.");
        return;
    }

    Bytes digest_bytes;
    if (!try_from_hex(digest, digest_bytes) || digest_bytes.size() != 32) {
        add_error(report, codes::SignatureInvalid, "The computed content digest is not a valid SHA-256 value.");
        return;
    }

    try {
        verify_signature(report.publisher_certificate.public_key(), signature.algorithm, digest_bytes, raw_signature);
    } catch (const Error& error) {
        add_error(report, error);
    }
}

void verify_inner_appx(Container& container, const ValidateOptions& options, Report& report) {
    const Manifest& manifest = container.manifest();

    // 若调用方提供了「已解包的主包」，校验它的字节与清单声明一致（防 TOCTOU），
    // 并且直接用它做后续内部校验 —— 因为部署栈最终装的就是这个文件。
    Bytes extracted;
    if (!options.extracted_main_package_path.empty()) {
        try {
            extracted = read_file(options.extracted_main_package_path);
        } catch (const Error& error) {
            add_error(report, error);
            return;
        }

        if (extracted.size() != manifest.main_package.size) {
            add_error(report, codes::SizeMismatch, "The extracted main package size does not match the manifest.",
                      manifest.main_package.path);
            return;
        }

        const std::string actual = sha256_hex(extracted);
        if (actual != manifest.main_package.sha256) {
            add_error(report, codes::HashMismatch, "The extracted main package hash does not match the manifest.",
                      manifest.main_package.path);
            return;
        }
    }

    zip::Archive inner = [&]() {
        if (!extracted.empty()) {
            return zip::Archive::open_memory(extracted.data(), extracted.size());
        }
        const zip::Entry* entry = container.archive().find(manifest.main_package.path);
        if (entry == nullptr) {
            fail(codes::FileMissing, "The main package entry is missing in the container.");
        }
        return container.open_nested(*entry);
    }();

    AppxIdentity appx;
    try {
        appx = read_appx_identity(inner);
    } catch (const Error& error) {
        add_error(report, error);
        return;
    }

    if (appx.name != manifest.identity.name) {
        add_error(report, codes::AppxInvalid, "The inner MSIX identity name does not match the container manifest.",
                  appx.name + " != " + manifest.identity.name);
    }

    if (appx.publisher != manifest.identity.publisher) {
        add_error(report, codes::AppxPublisherMismatch,
                  "The inner MSIX publisher does not match the container manifest.",
                  appx.publisher + " != " + manifest.identity.publisher);
    }

    if (normalize_appx_version(appx.version) != manifest.identity.version) {
        add_error(report, codes::AppxVersionMismatch,
                  "The inner MSIX version does not match the container manifest.",
                  appx.version + " != " + manifest.identity.version);
    }

    if (normalize_appx_architecture(appx.architecture) != manifest.identity.architecture) {
        add_error(report, codes::AppxArchitectureMismatch,
                  "The inner MSIX architecture does not match the container manifest.",
                  appx.architecture + " != " + manifest.identity.architecture);
    }

    for (const FileRef& dependency : manifest.dependencies) {
        const bool declared = std::any_of(appx.dependency_names.begin(), appx.dependency_names.end(),
                                          [&dependency](const std::string& name) {
                                              return equals_ignore_case_ascii(name, dependency.name);
                                          });
        if (!declared) {
            add_error(report, codes::AppxInvalid,
                      "The container carries a dependency that the inner MSIX does not declare.", dependency.name);
        }
    }

    if (!manifest.has_publisher) {
        return;
    }

    std::vector<Certificate> certificates;
    try {
        certificates = read_appx_signer_certificates(inner);
    } catch (const Error& error) {
        add_error(report, error);
        return;
    }

    const bool matched = std::any_of(certificates.begin(), certificates.end(), [&manifest](const Certificate& candidate) {
        return candidate.sha256_thumbprint() == manifest.publisher.thumbprint_sha256;
    });

    if (!matched) {
        add_error(report, codes::AppxSignerMismatch,
                  "The inner MSIX is signed by a different certificate than the one carried in the container.",
                  manifest.publisher.thumbprint_sha256);
    }
}

Report validate_impl(std::function<Container()> open_container, const ValidateOptions& options) {
    Report report;

    try {
        Container container = open_container();

        const Manifest& manifest = container.manifest();
        report.manifest = manifest;
        report.has_manifest = true;

        if (!options.installer_version.empty() && !version_at_least(options.installer_version, manifest.installer_min_version)) {
            add_error(report, codes::InstallerTooOld, "This installer is older than the minimum required by the package.",
                      manifest.installer_min_version + " > " + options.installer_version);
        }

        if (options.host_build != 0 && options.host_build < manifest.min_os_build) {
            add_error(report, codes::OsTooOld, "The operating system is older than the minimum required by the package.",
                      std::to_string(manifest.min_os_build) + " > " + std::to_string(options.host_build));
        }

        if (!options.host_architecture.empty() &&
            !is_architecture_compatible(manifest.identity.architecture, options.host_architecture)) {
            add_error(report, codes::ArchitectureMismatch,
                      "The package architecture is not compatible with this machine.",
                      manifest.identity.architecture + " vs " + options.host_architecture);
        }

        verify_declared_files(container, report);
        verify_publisher_certificate(container, options, report);
        verify_container_signature(container, options, report);

        if (options.validate_inner_appx && report.errors.empty()) {
            verify_inner_appx(container, options, report);
        }
    } catch (const Error& error) {
        add_error(report, error);
    } catch (const std::exception& ex) {
        add_error(report, codes::IoError, "An unexpected error occurred while validating the container.", ex.what());
    }

    return report;
}

}  // namespace

std::string Report::summary() const {
    if (valid()) {
        return warnings.empty() ? "valid" : "valid (" + std::to_string(warnings.size()) + " warning(s))";
    }

    std::string result;
    for (size_t i = 0; i < errors.size() && i < 4; ++i) {
        if (i > 0) {
            result += "; ";
        }
        result += errors[i].to_string();
    }
    if (errors.size() > 4) {
        result += "; …(" + std::to_string(errors.size() - 4) + " more)";
    }
    return result;
}

Report validate(const std::filesystem::path& container_path, const ValidateOptions& options) {
    return validate_impl([&container_path]() { return Container::open(container_path); }, options);
}

Report validate_memory(const uint8_t* data, size_t size, const ValidateOptions& options) {
    return validate_impl([data, size]() {
        zip::Archive archive = zip::Archive::open_memory(data, size);
        const zip::Entry* manifest_entry = archive.find("msixpkg.json");
        if (archive.entries().empty() || archive.entries().front().name != "msixpkg.json" || manifest_entry == nullptr) {
            fail(codes::ManifestNotFirst, "msixpkg.json must be present and be the first entry in the container.");
        }
        const Bytes manifest_bytes = archive.read_to_memory(*manifest_entry, 16ull * 1024 * 1024);
        const Manifest manifest = Manifest::parse(
            std::string_view(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()));
        check_container_structure(archive, manifest);
        return Container::adopt(std::move(archive), manifest);
    }, options);
}

}  // namespace msixpkg
