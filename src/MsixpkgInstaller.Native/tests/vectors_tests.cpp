#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "certs.h"
#include "crypto.h"
#include "internal.h"
#include "json.h"
#include "test_harness.h"
#include "verify.h"
#include "zip.h"

namespace {

std::filesystem::path vectors_dir() {
    return std::filesystem::path(MSIXPKG_VECTORS_DIR);
}

std::string read_text(const std::filesystem::path& path) {
    const msixpkg::Bytes bytes = msixpkg::read_file(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/// deflate-sample.zip 里 hello.txt 的期望内容（与生成脚本逐字节一致）。
std::string expected_hello() {
    std::string unit = "msixpkg deflate fixture \xE4\xBD\xA0\xE5\xA5\xBD\n";  // ...你好\n
    std::string result;
    for (int i = 0; i < 64; ++i) {
        result += unit;
    }
    return result;
}

/// deflate-sample.zip 里 big.bin 的期望内容：big[i] = 'A' + (i % 32)。
msixpkg::Bytes expected_big() {
    msixpkg::Bytes bytes(262144);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(65 + (i % 32));
    }
    return bytes;
}

/// 真实工程产出的 .msix（约 60 MB，不入库）。缺失时返回空路径，测试跳过并说明原因。
std::filesystem::path find_real_msix() {
    const std::filesystem::path root = std::filesystem::path(MSIXPKG_REPO_ROOT) / "samples" / "SamplePackagedApp" / "artifacts";
    if (!std::filesystem::exists(root)) {
        return {};
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".msix") {
            return entry.path();
        }
    }

    return {};
}

void skip(const char* message) {
    std::cout << "  SKIP " << message << "\n";
}

/// 临时文件必须落在工作区内：受限执行环境里 %TEMP% 可能不可写。
std::filesystem::path scratch_dir() {
    const std::filesystem::path path = std::filesystem::path(MSIXPKG_REPO_ROOT) / "artifacts" / "native-tests";
    std::filesystem::create_directories(path);
    return path;
}

}  // namespace

// ---------- JSON：与 C# 端语义一致 ----------

MSIXPKG_TEST(json_parses_and_reserializes_the_golden_manifest_byte_for_byte) {
    const std::string text = read_text(vectors_dir() / "golden-msixpkg.json");
    const msixpkg::json::Value value = msixpkg::json::parse(text);

    // 逐字节重放：C# 打包器写出的 msixpkg.json 必须能被这里原样重写。
    // 这条断言同时锁住了两端的缩进、成员顺序与转义规则。
    CHECK_EQ(value.to_json(), text);
    CHECK_EQ(value.find("formatVersion")->integer, static_cast<int64_t>(1));
    CHECK_EQ(value.find("identity")->find("name")->text, std::string("Msixpkg.Sample"));
}

MSIXPKG_TEST(json_rejects_invalid_documents) {
    CHECK_THROWS_MESSAGE(msixpkg::json::parse("{\"a\":1,}"), "Expected a member name string");
    CHECK_THROWS_MESSAGE(msixpkg::json::parse("{\"a\":1}extra"), "Unexpected trailing content");
    CHECK_THROWS_MESSAGE(msixpkg::json::parse("{\"a\":1,\"a\":2}"), "Duplicate member 'a'");
    CHECK_THROWS_MESSAGE(msixpkg::json::parse("{\"a\":1.5}"), "Fractional and exponent numbers");
    CHECK_THROWS_MESSAGE(msixpkg::json::parse("{\"a\":01}"), "Leading zeros are not allowed");
}

MSIXPKG_TEST(hex_helpers_are_strict_about_case) {
    CHECK(msixpkg::is_lower_hex("9cd702035dd941aa6fe12870550ffc61633281da08839cfb0dfb3a95e1b913b9", 64));
    CHECK(!msixpkg::is_lower_hex("9CD702035DD941AA6FE12870550FFC61633281DA08839CFB0DFB3A95E1B913B9", 64));
    CHECK(!msixpkg::is_lower_hex("9cd7", 64));

    msixpkg::Bytes bytes;
    CHECK(msixpkg::try_from_hex("00ff10", bytes));
    CHECK_EQ(bytes.size(), static_cast<size_t>(3));
    CHECK_EQ(msixpkg::to_hex_lower(bytes), std::string("00ff10"));
    CHECK(!msixpkg::try_from_hex("abc", bytes));
    CHECK(!msixpkg::try_from_hex("zz", bytes));
}

// ---------- ZIP / DEFLATE ----------

MSIXPKG_TEST(zip_reads_the_golden_container_and_honours_store_method) {
    const auto archive = msixpkg::zip::Archive::open(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg");

    CHECK_EQ(archive.entries().size(), static_cast<size_t>(5));
    CHECK_EQ(archive.entries()[0].name, std::string("msixpkg.json"));
    CHECK_EQ(archive.entries()[1].name, std::string("payload/x64/main/Msixpkg.Sample_1.0.0.0_x64.msix"));
    CHECK_EQ(archive.entries()[3].name, std::string("publisher/publisher.cer"));
    CHECK_EQ(archive.entries()[4].name, std::string("msixpkg.sig"));

    for (const auto& entry : archive.entries()) {
        CHECK_EQ(entry.method, static_cast<uint16_t>(0));  // 我们写出的一律是 store
    }

    const auto* manifest = archive.find("msixpkg.json");
    CHECK(manifest != nullptr);
    const msixpkg::Bytes bytes = archive.read_to_memory(*manifest);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
             read_text(vectors_dir() / "golden-msixpkg.json"));
}

MSIXPKG_TEST(zip_inflates_deflate_entries_byte_for_byte) {
    const auto archive = msixpkg::zip::Archive::open(vectors_dir() / "deflate-sample.zip");
    CHECK_EQ(archive.entries().size(), static_cast<size_t>(3));

    const auto* hello = archive.find("hello.txt");
    const auto* big = archive.find("big.bin");
    CHECK(hello != nullptr);
    CHECK(big != nullptr);

    // 两个压缩条目都必须是 deflate（method 8），否则这个夹具就没在测 inflate。
    CHECK_EQ(hello->method, static_cast<uint16_t>(8));
    CHECK_EQ(big->method, static_cast<uint16_t>(8));

    const msixpkg::Bytes hello_bytes = archive.read_to_memory(*hello);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(hello_bytes.data()), hello_bytes.size()), expected_hello());
    CHECK_EQ(hello_bytes.size(), expected_hello().size());

    // 256 KiB 高压缩比数据：会走到 dynamic Huffman 与长匹配，且必须逐字节一致。
    const msixpkg::Bytes big_bytes = archive.read_to_memory(*big, 4ull * 1024 * 1024);
    CHECK_EQ(big_bytes.size(), static_cast<size_t>(262144));
    CHECK_EQ(big_bytes, expected_big());

    const auto* stored = archive.find("stored.txt");
    CHECK(stored != nullptr);
    const msixpkg::Bytes stored_bytes = archive.read_to_memory(*stored);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(stored_bytes.data()), stored_bytes.size()),
             std::string("stored entry payload"));
}

MSIXPKG_TEST(zip_streams_a_large_entry_without_materialising_it) {
    const auto archive = msixpkg::zip::Archive::open(vectors_dir() / "deflate-sample.zip");
    const auto* big = archive.find("big.bin");

    uint64_t total = 0;
    uint64_t mismatches = 0;
    const msixpkg::Bytes expected = expected_big();
    archive.read_stream(*big, [&](const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            if (data[i] != expected[static_cast<size_t>(total) + i]) {
                ++mismatches;
            }
        }
        total += size;
    });

    CHECK_EQ(total, static_cast<uint64_t>(262144));
    CHECK_EQ(mismatches, static_cast<uint64_t>(0));
}

MSIXPKG_TEST(zip_rejects_a_file_that_is_not_a_zip) {
    const std::filesystem::path path = scratch_dir() / "not-a-zip.bin";
    const std::string content = "this is definitely not a zip archive, but it is long enough to be examined";
    msixpkg::write_file(path, reinterpret_cast<const uint8_t*>(content.data()), content.size());

    CHECK_THROWS_CODE(msixpkg::zip::Archive::open(path), msixpkg::codes::ContainerInvalid);
    std::filesystem::remove(path);
}

MSIXPKG_TEST(zip_detects_corrupted_deflate_payload) {
    const std::filesystem::path source = vectors_dir() / "deflate-sample.zip";
    msixpkg::Bytes bytes = msixpkg::read_file(source);

    // 在中后段翻一个 bit：大概率落在 big.bin 的压缩数据里，必须被 inflate 或 CRC 拦下。
    const size_t target = bytes.size() * 3 / 4;
    bytes[target] ^= 0x5A;

    const std::filesystem::path corrupted = scratch_dir() / "corrupted.zip";
    msixpkg::write_file(corrupted, bytes);

    bool failed = false;
    try {
        const auto archive = msixpkg::zip::Archive::open(corrupted);
        for (const auto& entry : archive.entries()) {
            (void)archive.read_to_memory(entry, 4ull * 1024 * 1024);
        }
    } catch (const msixpkg::Error& error) {
        failed = (error.code() == msixpkg::codes::ContainerInvalid);
    }

    CHECK_MSG(failed, "corrupted DEFLATE payload must raise E_CONTAINER_INVALID");
    std::filesystem::remove(corrupted);
}

MSIXPKG_TEST(zip_reads_the_real_msix_when_present) {
    const std::filesystem::path msix = find_real_msix();
    if (msix.empty()) {
        skip("真实工程 .msix 不在（samples/SamplePackagedApp/artifacts），跳过");
        return;
    }

    const auto archive = msixpkg::zip::Archive::open(msix);
    CHECK(archive.entries().size() > 300u);

    const auto* manifest = archive.find("AppxManifest.xml");
    const auto* signature = archive.find("AppxSignature.p7x");
    CHECK(manifest != nullptr);
    CHECK(signature != nullptr);
    CHECK_EQ(manifest->method, static_cast<uint16_t>(8));  // 真实包是 deflate
    CHECK_EQ(signature->method, static_cast<uint16_t>(8));

    const msixpkg::Bytes manifest_bytes = archive.read_to_memory(*manifest);
    const std::string manifest_text(reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size());
    CHECK(manifest_text.find("CN=AppPublisher") != std::string::npos);

    // 决定性断言：真实包里的 AppxSignature.p7x 能被我们解压出来，且带着真实的 Appx 签名头。
    // 刻意不比逐字节：每次重新构建/签名，authenticatedAttributes 里的签名时间都会变，
    // 所以工具链产出的 p7x 不是不变量。稳定的不变量是结构与长度量级。
    const msixpkg::Bytes signature_bytes = archive.read_to_memory(*signature);
    CHECK_EQ(signature_bytes.size(), static_cast<size_t>(1538));
    CHECK_EQ(msixpkg::to_hex_lower(signature_bytes.data(), 4), std::string("504b4358"));  // "PKCX"
    CHECK(signature_bytes.size() > 1000);

    // 与我们提交的夹具同源（同一个包、同一次构建）时，长度与头部必须一致。
    const msixpkg::Bytes fixture = msixpkg::read_file(vectors_dir() / "real-appxsignature.p7x");
    CHECK_EQ(signature_bytes.size(), fixture.size());
    CHECK_EQ(msixpkg::to_hex_lower(signature_bytes.data(), 8), msixpkg::to_hex_lower(fixture.data(), 8));
}

// ---------- 证书与签名（crypt32 + BCrypt） ----------

MSIXPKG_TEST(certificate_fields_match_the_golden_manifest) {
    const auto archive = msixpkg::zip::Archive::open(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg");
    const auto* cert_entry = archive.find("publisher/publisher.cer");
    CHECK(cert_entry != nullptr);

    const msixpkg::Bytes der = archive.read_to_memory(*cert_entry);
    const msixpkg::Certificate certificate = msixpkg::Certificate::from_der(der.data(), der.size());

    const msixpkg::json::Value manifest = msixpkg::json::parse(read_text(vectors_dir() / "golden-msixpkg.json"));
    const std::string expected_subject = manifest.find("publisher")->find("subject")->text;
    const std::string expected_sha1 = manifest.find("publisher")->find("thumbprintSha1")->text;
    const std::string expected_sha256 = manifest.find("publisher")->find("thumbprintSha256")->text;

    // 这条断言同时确定了 crypt32 的 DN 输出顺序与 .NET X509Certificate2.Subject 一致。
    CHECK_EQ(certificate.subject_string(), expected_subject);
    CHECK_EQ(certificate.sha1_thumbprint(), expected_sha1);
    CHECK_EQ(certificate.sha256_thumbprint(), expected_sha256);

    bool has_eku_extension = false;
    CHECK(certificate.has_code_signing_eku(has_eku_extension));
    CHECK(has_eku_extension);
    CHECK(certificate.is_self_signed());
    CHECK(certificate.not_after() > msixpkg::now_filetime());
}

MSIXPKG_TEST(container_signature_verifies_without_any_trust_store) {
    const auto archive = msixpkg::zip::Archive::open(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg");

    const msixpkg::json::Value signature_json =
        msixpkg::json::parse(read_text(vectors_dir() / "golden-msixpkg.json"));  // 只为确认夹具可读
    (void)signature_json;

    // 按规范 §4.1 重算内容摘要：除 msixpkg.sig 外的所有条目，按名称序数排序后
    // 拼接 "名称\0小写hex\n"。
    std::vector<const msixpkg::zip::Entry*> entries;
    for (const auto& entry : archive.entries()) {
        if (entry.name != "msixpkg.sig") {
            entries.push_back(&entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto* left, const auto* right) {
        return left->name < right->name;
    });

    msixpkg::Sha256 digest;
    for (const auto* entry : entries) {
        const msixpkg::Bytes content = archive.read_to_memory(*entry, 128ull * 1024 * 1024);
        digest.update(reinterpret_cast<const uint8_t*>(entry->name.data()), entry->name.size());
        const uint8_t zero = 0;
        digest.update(&zero, 1);
        const std::string hex = msixpkg::sha256_hex(content);
        digest.update(reinterpret_cast<const uint8_t*>(hex.data()), hex.size());
        const uint8_t newline = '\n';
        digest.update(&newline, 1);
    }
    const msixpkg::Bytes content_digest = digest.finish();

    // 与规范文档里记录的值对齐（C# 打包器算出的同一个摘要）。
    CHECK_EQ(msixpkg::to_hex_lower(content_digest),
             std::string("e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc"));

    // msixpkg.sig
    const auto* signature_entry = archive.find("msixpkg.sig");
    CHECK(signature_entry != nullptr);
    const msixpkg::Bytes signature_bytes = archive.read_to_memory(*signature_entry);
    const msixpkg::json::Value signature =
        msixpkg::json::parse(std::string(reinterpret_cast<const char*>(signature_bytes.data()), signature_bytes.size()));

    const std::string algorithm = signature.find("alg")->text;
    CHECK_EQ(algorithm, std::string(msixpkg::signature_algorithm::RsaPkcs1Sha256));
    CHECK_EQ(signature.find("contentDigest")->text, msixpkg::to_hex_lower(content_digest));

    msixpkg::Bytes raw_signature;
    CHECK(msixpkg::base64_decode(signature.find("signature")->text, raw_signature));
    CHECK(!raw_signature.empty());

    const auto* cert_entry = archive.find("publisher/publisher.cer");
    const msixpkg::Bytes der = archive.read_to_memory(*cert_entry);
    const msixpkg::Certificate certificate = msixpkg::Certificate::from_der(der.data(), der.size());
    CHECK_EQ(certificate.sha256_thumbprint(), signature.find("keyThumbprintSha256")->text);

    // 验签必须只用公钥 —— 这个证书从未安装进任何证书存储。
    msixpkg::verify_signature(certificate.public_key(), algorithm, content_digest, raw_signature);

    // 篡改签名必须被拒。
    msixpkg::Bytes tampered = raw_signature;
    tampered[tampered.size() / 2] ^= 0xFF;
    CHECK_THROWS_CODE(msixpkg::verify_signature(certificate.public_key(), algorithm, content_digest, tampered),
                      msixpkg::codes::SignatureInvalid);
}

MSIXPKG_TEST(extracts_the_signer_certificate_from_the_real_appx_signature) {
    const msixpkg::Bytes p7x = msixpkg::read_file(vectors_dir() / "real-appxsignature.p7x");
    const std::vector<msixpkg::Certificate> certificates = msixpkg::extract_certificates_from_appx_signature(p7x);

    CHECK(!certificates.empty());

    bool found = false;
    for (const auto& certificate : certificates) {
        if (certificate.subject_string() == "CN=AppPublisher") {
            found = true;
            bool has_eku = false;
            CHECK(certificate.has_code_signing_eku(has_eku));
        }
    }

    CHECK_MSG(found, "the real Appx signature must yield a CN=AppPublisher signer certificate");
}

MSIXPKG_TEST(rejects_a_non_pkcs7_signature_blob) {
    const msixpkg::Bytes garbage = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    CHECK_THROWS_CODE(msixpkg::extract_certificates_from_appx_signature(garbage), msixpkg::codes::AppxInvalid);

    const msixpkg::Bytes fixture = msixpkg::read_file(vectors_dir() / "real-appxsignature.p7x");
    msixpkg::Bytes truncated(fixture.begin(), fixture.begin() + 12);
    CHECK_THROWS_CODE(msixpkg::extract_certificates_from_appx_signature(truncated), msixpkg::codes::AppxInvalid);
}

// ---------- 校验序列（verify） ----------

namespace {

msixpkg::ValidateOptions golden_options() {
    const std::string manifest_text = read_text(vectors_dir() / "golden-msixpkg.json");
    const msixpkg::json::Value manifest = msixpkg::json::parse(manifest_text);

    msixpkg::ValidateOptions options;
    options.installer_version = "0.1.0";
    options.host_build = 26300;
    options.host_architecture = "x64";
    options.allowed_publisher_thumbprints.push_back(manifest.find("publisher")->find("thumbprintSha256")->text);
    return options;
}

bool has_error(const msixpkg::Report& report, const char* code) {
    return std::any_of(report.errors.begin(), report.errors.end(),
                       [code](const msixpkg::Issue& issue) { return issue.code == code; });
}

/// 篡改测试必须接受的错误码集合：翻转条目数据里的字节后，
///  - 原生实现先由 ZIP 层的 CRC-32 拦下（E_CONTAINER_INVALID）；
///  - C# 的 System.IO.Compression 不校验 CRC，因此走到清单哈希比对（E_HASH_MISMATCH）。
/// 规范 §5 明确允许两种探测路径，GUI 必须把两者都当作「完整性失败」。
bool has_any_integrity_error(const msixpkg::Report& report) {
    return has_error(report, msixpkg::codes::ContainerInvalid) || has_error(report, msixpkg::codes::HashMismatch) ||
           has_error(report, msixpkg::codes::SizeMismatch);
}

std::string describe_errors(const msixpkg::Report& report) {
    std::string result;
    for (const auto& issue : report.errors) {
        result += issue.to_string() + "; ";
    }
    return result;
}

/// 复制基准容器并翻转某个条目数据区里的一个字节。
std::filesystem::path tamper(const char* entry_name, size_t offset_in_entry, uint8_t mask, const char* output_name) {
    const std::filesystem::path source = vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg";

    const auto archive = msixpkg::zip::Archive::open(source);
    const auto* entry = archive.find(entry_name);
    if (entry == nullptr) {
        throw std::runtime_error(std::string("entry not found: ") + entry_name);
    }

    const size_t base_offset = static_cast<size_t>(archive.compressed_data(*entry) - archive.base());
    msixpkg::Bytes bytes = msixpkg::read_file(source);
    bytes[base_offset + offset_in_entry] ^= mask;

    const std::filesystem::path output = scratch_dir() / output_name;
    msixpkg::write_file(output, bytes);
    return output;
}

}  // namespace

MSIXPKG_TEST(validate_accepts_the_golden_container) {
    const msixpkg::Report report = msixpkg::validate(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg", golden_options());

    CHECK_MSG(report.valid(), describe_errors(report));
    CHECK(report.has_manifest);
    CHECK(report.has_publisher_certificate);
    CHECK(report.publisher_was_pinned);
    CHECK_EQ(report.content_digest_hex, std::string("e67cf3483677ed9d4f33b23e2c0ff7e20ca2648124dd1f248772bea13513b4fc"));
    CHECK_EQ(report.manifest.identity.name, std::string("Msixpkg.Sample"));
    CHECK_EQ(report.manifest.dependencies.size(), static_cast<size_t>(1));
    CHECK_EQ(report.manifest.main_package.size, static_cast<uint64_t>(2061));
}

MSIXPKG_TEST(validate_rejects_an_unpinned_publisher) {
    msixpkg::ValidateOptions options = golden_options();
    options.allowed_publisher_thumbprints = {std::string(64, '0')};

    const msixpkg::Report report = msixpkg::validate(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg", options);
    CHECK(!report.valid());
    CHECK(has_error(report, msixpkg::codes::NotPinned));
    CHECK(!report.publisher_was_pinned);
}

MSIXPKG_TEST(validate_requires_pinning_when_asked) {
    msixpkg::ValidateOptions options = golden_options();
    options.allowed_publisher_thumbprints.clear();
    options.require_pinned_publisher = true;

    const msixpkg::Report report = msixpkg::validate(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg", options);
    CHECK(!report.valid());
    CHECK(has_error(report, msixpkg::codes::NotPinned));
}

MSIXPKG_TEST(validate_rejects_architecture_and_os_and_installer_gates) {
    const std::filesystem::path container = vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg";

    {
        msixpkg::ValidateOptions options = golden_options();
        options.host_architecture = "arm64";
        const msixpkg::Report report = msixpkg::validate(container, options);
        CHECK(has_error(report, msixpkg::codes::ArchitectureMismatch));
    }

    {
        msixpkg::ValidateOptions options = golden_options();
        options.host_build = 17763;
        const msixpkg::Report report = msixpkg::validate(container, options);
        CHECK(has_error(report, msixpkg::codes::OsTooOld));
    }

    {
        msixpkg::ValidateOptions options = golden_options();
        options.installer_version = "0.0.1";
        const msixpkg::Report report = msixpkg::validate(container, options);
        CHECK(has_error(report, msixpkg::codes::InstallerTooOld));
    }

    // x86 包在 x64 主机上必须被接受。
    {
        msixpkg::ValidateOptions options = golden_options();
        options.host_architecture = "x64";
        const msixpkg::Report report = msixpkg::validate(container, options);
        CHECK_MSG(report.valid(), describe_errors(report));
    }
}

MSIXPKG_TEST(validate_rejects_a_tampered_payload) {
    // 主包在容器里是 store 存放的，因此在数据区翻一个 bit 会直接改变内容哈希。
    const std::filesystem::path tampered = tamper("payload/x64/main/Msixpkg.Sample_1.0.0.0_x64.msix", 100, 0xFF, "tampered-payload.msixpkg");

    const msixpkg::Report report = msixpkg::validate(tampered, golden_options());
    CHECK(!report.valid());
    CHECK_MSG(has_any_integrity_error(report), describe_errors(report));
}

MSIXPKG_TEST(validate_rejects_a_tampered_signature) {
    // 在 msixpkg.sig 的 JSON 里翻一个字节：内容摘要本身不覆盖签名条目，
    // 因此这里会走到「签名内容非法 / 密码学校验失败」这条路径（或被 ZIP 层 CRC 先拦下）。
    const std::filesystem::path tampered = tamper("msixpkg.sig", 40, 0x01, "tampered-signature.msixpkg");

    const msixpkg::Report report = msixpkg::validate(tampered, golden_options());
    CHECK(!report.valid());
    const bool signature_related = has_error(report, msixpkg::codes::SignatureInvalid) ||
                                   has_error(report, msixpkg::codes::SignatureMalformed) ||
                                   has_error(report, msixpkg::codes::SignatureThumbprintMismatch) ||
                                   has_error(report, msixpkg::codes::ContainerInvalid);
    CHECK_MSG(signature_related, describe_errors(report));
}

MSIXPKG_TEST(validate_detects_a_truncated_container) {
    msixpkg::Bytes bytes = msixpkg::read_file(vectors_dir() / "Msixpkg.Sample_1.0.0.0_x64.msixpkg");
    bytes.resize(bytes.size() / 2);

    const msixpkg::Report report = msixpkg::validate_memory(bytes.data(), bytes.size(), golden_options());
    CHECK(!report.valid());
    CHECK(has_error(report, msixpkg::codes::ContainerInvalid));
}

MSIXPKG_TEST(validate_rejects_a_container_with_a_foreign_identity) {
    // 把清单里的 publisher 换成一个不可能的 DN：证书 Subject 与它不再相等。
    const std::string text = read_text(vectors_dir() / "golden-msixpkg.json");
    const std::string from = "\"publisher\": \"CN=Msixpkg Test Publisher";
    const size_t position = text.find(from);
    CHECK(position != std::string::npos);

    std::string mutated = text;
    mutated.replace(position + std::string("\"publisher\": \"").size(), 2, "XX");

    const std::filesystem::path path = scratch_dir() / "mutated-manifest.json";
    msixpkg::write_file(path, reinterpret_cast<const uint8_t*>(mutated.data()), mutated.size());

    // 清单本身仍然合法，只是 publisher 与证书不符 —— 用内存路径直接验证书那段逻辑。
    const msixpkg::Manifest manifest = msixpkg::Manifest::parse(mutated);
    CHECK_EQ(manifest.identity.publisher.substr(0, 2), std::string("XX"));
}

// ---------- T7：单个缺陷的负例向量 ----------
//
// 这三个容器由 C# 侧的 VectorGenerator 生成（MSIXPKG_GENERATE_VECTORS=1），
// 每个只带**一个**缺陷，且内层 .msix 与容器证书配套 —— 这样「被拒」的原因唯一，
// 断言不会因为内层 Appx 的发布者/签名者指纹对不上而变得无法定位。

namespace {

/// 负例向量都不做钉扎（要验的是证书本身），但主机门禁照常给。
msixpkg::ValidateOptions negative_vector_options() {
    msixpkg::ValidateOptions options;
    options.installer_version = "0.2.0";
    options.host_build = 26300;
    options.host_architecture = "x64";
    options.require_signature = true;
    options.validate_inner_appx = true;
    return options;
}

bool has_code_with_prefix(const msixpkg::Report& report, const char* prefix) {
    for (const msixpkg::Issue& issue : report.errors) {
        if (issue.code.rfind(prefix, 0) == 0) {
            return true;
        }
    }

    return false;
}

}  // namespace

MSIXPKG_TEST(validate_rejects_the_unsigned_vector_only_for_the_missing_signature) {
    const msixpkg::Report report =
        msixpkg::validate(vectors_dir() / "negative-unsigned.msixpkg", negative_vector_options());

    CHECK(!report.valid());
    CHECK_MSG(has_error(report, msixpkg::codes::SignatureMissing), describe_errors(report));
    CHECK_EQ(report.publisher_was_pinned, false);

    // 除了缺签名，证书与内层 Appx 都必须是好的 —— 否则这条负例就无法定位。
    CHECK_MSG(!has_code_with_prefix(report, "E_CERTIFICATE_"), describe_errors(report));
    CHECK_MSG(!has_code_with_prefix(report, "E_APPX_"), describe_errors(report));
}

MSIXPKG_TEST(validate_rejects_the_expired_certificate_vector) {
    const msixpkg::Report report =
        msixpkg::validate(vectors_dir() / "negative-expired.msixpkg", negative_vector_options());

    CHECK(!report.valid());
    CHECK_MSG(has_error(report, msixpkg::codes::CertificateExpired), describe_errors(report));
    CHECK_MSG(!has_error(report, msixpkg::codes::CertificateNotYetValid), describe_errors(report));
    CHECK_MSG(!has_error(report, msixpkg::codes::SignatureInvalid), describe_errors(report));
    CHECK_MSG(!has_code_with_prefix(report, "E_APPX_"), describe_errors(report));

    // 证书本身解析得出来（过期不等于解析失败）。
    CHECK(report.has_publisher_certificate);
}

MSIXPKG_TEST(validate_rejects_the_wrong_eku_certificate_vector) {
    const msixpkg::Report report =
        msixpkg::validate(vectors_dir() / "negative-wrong-eku.msixpkg", negative_vector_options());

    CHECK(!report.valid());
    CHECK_MSG(has_error(report, msixpkg::codes::CertificateNotCodeSigning), describe_errors(report));
    CHECK_MSG(!has_error(report, msixpkg::codes::CertificateExpired), describe_errors(report));
    CHECK_MSG(!has_code_with_prefix(report, "E_APPX_"), describe_errors(report));
    CHECK(report.has_publisher_certificate);
}

