// certstore 的单元测试。
//
// **绝不碰真实的 LocalMachine\TrustedPeople**：所有增 / 删都发生在进程内内存存储
// （`CERT_STORE_PROV_MEMORY`，与注册表存储走同一套 crypt32 CRUD 路径），
// 对真实存储只做只读查询。
//
// 为什么不用交接文档建议的 `CURRENT_USER + L"MsixpkgTest"`：实测本执行环境拒绝
// **任何** HKCU 注册表写操作（`RegCreateKeyEx` 与 `CertOpenStore` 写打开都返回 5），
// 用户级存储的增 / 删在这里根本无法运行。详见 certstore.h 顶部注释。

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include "certstore.h"
#include "container.h"
#include "crypto.h"
#include "internal.h"
#include "test_harness.h"

namespace {

struct PublisherCertificateFixture {
    msixpkg::Bytes der;
    msixpkg::Bytes sha1;
    std::string sha1_hex;
};

/// 取黄金容器里内嵌的发布者证书（真实的自签名测试证书，SHA-256 指纹 9cd70203…13b9）。
PublisherCertificateFixture load_publisher_certificate() {
    const std::filesystem::path container_path =
        std::filesystem::path(MSIXPKG_VECTORS_DIR) / "Msixpkg.Sample_1.0.0.0_x64.msixpkg";

    msixpkg::Container container = msixpkg::Container::open(container_path);
    const msixpkg::Manifest& manifest = container.manifest();
    if (!manifest.has_publisher) {
        throw std::runtime_error("the golden container has no embedded publisher certificate");
    }

    PublisherCertificateFixture fixture;
    fixture.der = container.read_entry("publisher/publisher.cer");
    fixture.sha1 = msixpkg::sha1(fixture.der.data(), fixture.der.size());
    fixture.sha1_hex = manifest.publisher.thumbprint_sha1;
    return fixture;
}

std::string upper_hex(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

}  // namespace

MSIXPKG_TEST(certstore_first_import_reports_added_then_reimport_is_idempotent) {
    const PublisherCertificateFixture fixture = load_publisher_certificate();
    CHECK_EQ(fixture.sha1_hex, msixpkg::to_hex_lower(fixture.sha1));

    msixpkg::CertificateStore store = msixpkg::CertificateStore::open_memory();
    CHECK(!store.contains(fixture.sha1));

    // 验收 1：新增 → existed_before == false（卸载时才有权回收这张证书）。
    const msixpkg::AddResult first = store.add(fixture.der, fixture.sha1);
    CHECK(first.added);
    CHECK(!first.existed_before);

    // 验收 3 的前半：查询命中（大小写十六进制都要能查到）。
    CHECK(store.contains(fixture.sha1));
    CHECK(store.contains_hex(fixture.sha1_hex));
    CHECK(store.contains_hex(upper_hex(fixture.sha1_hex)));

    // 验收 2：再新增同一证书 → added == false, existed_before == true。
    const msixpkg::AddResult second = store.add(fixture.der, fixture.sha1);
    CHECK(!second.added);
    CHECK(second.existed_before);
    CHECK(store.contains(fixture.sha1));
}

MSIXPKG_TEST(certstore_remove_deletes_the_entry_and_is_idempotent) {
    const PublisherCertificateFixture fixture = load_publisher_certificate();
    msixpkg::CertificateStore store = msixpkg::CertificateStore::open_memory();

    store.add(fixture.der, fixture.sha1);
    CHECK(store.contains(fixture.sha1));

    // 验收 3 的后半：删除后查询未命中。
    CHECK(store.remove(fixture.sha1));
    CHECK(!store.contains(fixture.sha1));

    // 重复删除：未命中不是错误，返回 false。
    CHECK(!store.remove(fixture.sha1));
    CHECK(!store.remove_hex(fixture.sha1_hex));

    // 删掉之后重新导入必须再次被识别为「本次新增」——否则卸载会误判所有权。
    const msixpkg::AddResult again = store.add(fixture.der, fixture.sha1);
    CHECK(again.added);
    CHECK(!again.existed_before);

    // 十六进制入口与字节入口必须操作同一张证书。
    CHECK(store.remove_hex(fixture.sha1_hex));
    CHECK(!store.contains(fixture.sha1));
}

MSIXPKG_TEST(certstore_rejects_malformed_certificate_without_residue) {
    msixpkg::CertificateStore store = msixpkg::CertificateStore::open_memory();

    const msixpkg::Bytes garbage{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    const msixpkg::Bytes garbage_sha1 = msixpkg::sha1(garbage.data(), garbage.size());

    // 验收 4：非证书字节 → E_CERTIFICATE_MALFORMED，且存储里不留残留。
    CHECK_THROWS_CODE(store.add(garbage, garbage_sha1), msixpkg::codes::CertificateMalformed);
    CHECK(!store.contains(garbage_sha1));

    // 空字节同样拒绝（且不能因为长度为 0 就越界）。
    CHECK_THROWS_CODE(store.add(msixpkg::Bytes{}, garbage_sha1), msixpkg::codes::CertificateMalformed);

    // 截断的真实证书也要拒绝，且不能因为「前半段是合法 DER 前缀」就写进去。
    const PublisherCertificateFixture fixture = load_publisher_certificate();
    const msixpkg::Bytes truncated(fixture.der.begin(), fixture.der.begin() + fixture.der.size() / 2);
    CHECK_THROWS_CODE(store.add(truncated, fixture.sha1), msixpkg::codes::CertificateMalformed);
    CHECK(!store.contains(fixture.sha1));
}

MSIXPKG_TEST(certstore_rejects_thumbprint_that_does_not_match_the_certificate) {
    const PublisherCertificateFixture fixture = load_publisher_certificate();
    msixpkg::CertificateStore store = msixpkg::CertificateStore::open_memory();

    // 「记下的指纹」与「实际写进去的证书」不一致时必须拒绝：
    // 否则状态库会记着 A、信任区里躺着 B，卸载时删错对象或删不掉。
    const msixpkg::Bytes wrong(20, 0xAB);
    CHECK_THROWS_CODE(store.add(fixture.der, wrong), msixpkg::codes::CertificateMalformed);
    CHECK(!store.contains(wrong));
    CHECK(!store.contains(fixture.sha1));

    // 指纹长度必须是 20 字节（SHA-1）。
    CHECK_THROWS_CODE(store.add(fixture.der, msixpkg::Bytes(19, 0)), msixpkg::codes::CertificateMalformed);
    CHECK_THROWS_CODE(store.add(fixture.der, msixpkg::Bytes(21, 0)), msixpkg::codes::CertificateMalformed);
    CHECK_THROWS_CODE(store.contains(msixpkg::Bytes(3, 0)), msixpkg::codes::CertificateMalformed);
    CHECK_THROWS_CODE(store.contains_hex("not-hex"), msixpkg::codes::CertificateMalformed);
    CHECK_THROWS_CODE(store.contains_hex("abcd"), msixpkg::codes::CertificateMalformed);
    CHECK_THROWS_CODE(store.remove(msixpkg::Bytes(0, 0)), msixpkg::codes::CertificateMalformed);
}

MSIXPKG_TEST(certstore_read_only_handle_allows_query_but_refuses_writes) {
    const PublisherCertificateFixture fixture = load_publisher_certificate();
    msixpkg::CertificateStore readonly_store = msixpkg::CertificateStore::open_memory(/*read_only=*/true);

    CHECK(readonly_store.read_only());
    CHECK(!readonly_store.contains(fixture.sha1));

    // 只读句柄绝不允许改动存储：这是「查询路径不应该去申请写权限」的代码级保证。
    CHECK_THROWS_CODE(readonly_store.add(fixture.der, fixture.sha1), msixpkg::codes::IoError);
    CHECK_THROWS_CODE(readonly_store.remove(fixture.sha1), msixpkg::codes::IoError);
    CHECK(!readonly_store.contains(fixture.sha1));

    // 同样的 20 字节指纹在只读句柄上走 remove：先过我们的只读门禁，不落到 crypt32。
    const msixpkg::Bytes wrong(20, 0x01);
    CHECK_THROWS_CODE(readonly_store.remove(wrong), msixpkg::codes::IoError);
}

MSIXPKG_TEST(certstore_machine_target_is_pinned_to_local_machine_trusted_people) {
    // 生产查询路径固定 LocalMachine\TrustedPeople，且默认只读 —— 红线的类型级保证：
    // 调用方拿不到任何写 `Root` 的可能性（位置是枚举，不是字符串）。
    msixpkg::CertificateStore store = msixpkg::CertificateStore::open_machine_trusted_people(true);
    CHECK(store.is_open());
    CHECK(store.read_only());
    CHECK(store.display_name() == L"LocalMachine\\TrustedPeople");

    // 一个不会存在的指纹：只读查询真实存储，不写入任何东西。
    const char probe[] = "msixpkg-absent-thumbprint-probe";
    const msixpkg::Bytes absent = msixpkg::sha1(reinterpret_cast<const uint8_t*>(probe), sizeof(probe) - 1);
    CHECK(!store.contains(absent));
    CHECK(!msixpkg::is_publisher_trusted(absent));
    CHECK(!msixpkg::is_publisher_trusted_hex(msixpkg::to_hex_lower(absent)));

    // 十六进制形式的指纹校验同样严格（这一步在打开存储前就会失败）。
    CHECK_THROWS_CODE(msixpkg::untrust_publisher_hex("zz"), msixpkg::codes::CertificateMalformed);
}
