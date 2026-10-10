#include "test_framework.hpp"
#include "archive_fixtures.hpp"
#include "file_test_support.hpp"
#include "app/update_check.hpp"
#include "app/updater.hpp"
#include "common/sha256.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace hub::app;
using namespace hub::test::archive_fixtures;
using namespace hub::test::file_support;
namespace fs = std::filesystem;

namespace {

constexpr Version kNext{CURRENT_VERSION.major, CURRENT_VERSION.minor,
                        static_cast<uint16_t>(CURRENT_VERSION.patch + 1)};

const std::string kDownloadUrl =
    std::string(RELEASE_DOWNLOAD_PREFIX) + "v" + kNext.to_string() + "/" + std::string(RELEASE_ASSET_NAME);
const std::string kPageUrl = std::string(RELEASE_PAGE_PREFIX) + "tag/v" + kNext.to_string();

std::string sha256_of(std::span<const uint8_t> bytes) {
    return hub::common::to_hex(hub::common::Sha256::hash(bytes));
}

std::string sha256_digest(std::span<const uint8_t> bytes) {
    return "sha256:" + sha256_of(bytes);
}

/// What GitHub's /releases/latest returns, trimmed to the fields read and one
/// other asset.
std::string release_json(const Version& version, const std::string& digest,
                         const std::string& url = kDownloadUrl, const std::string& page = kPageUrl) {
    return R"({"url":"https://api.github.com/repos/mgauna-ar/ffxiv-hub/releases/1",)"
           R"("html_url":")" + page + R"(","tag_name":"v)" + version.to_string() +
           R"(","name":"v)" + version.to_string() + R"(","draft":false,"prerelease":false,)"
           R"("body":"## What's Changed\n* a fix é",)"
           R"("assets":[{"name":"something-else.txt","size":3,)"
           R"("browser_download_url":")" + std::string(RELEASE_DOWNLOAD_PREFIX) + R"(v1/something-else.txt",)"
           R"("digest":"sha256:0000000000000000000000000000000000000000000000000000000000000000"},)"
           R"({"name":")" + std::string(RELEASE_ASSET_NAME) + R"(","size":2130658,)"
           R"("browser_download_url":")" + url + R"(","digest":")" + digest + R"("}]})";
}

/// Stands in for GitHub: the API's answer and the archive, by URL. Fetches run
/// on the updater's worker, so it must outlive the Updater.
struct FakeGitHub {
    bool offline{false};
    int api_status{200};
    std::string api_body;
    int download_status{200};
    std::vector<uint8_t> archive;
    std::atomic<int> api_calls{0};
    std::atomic<int> download_calls{0};
    std::atomic<bool> progress_reported{false};

    /// Offers kNext as `zip`, with `digest` published for it (the real one if empty).
    void offer(std::span<const uint8_t> zip, std::string digest = {}) {
        archive.assign(zip.begin(), zip.end());
        api_body = release_json(kNext, digest.empty() ? sha256_digest(zip) : digest);
    }

    Updater::Fetch fetch() {
        return [this](const hub::os::HttpRequest& request, std::string* error) -> std::optional<hub::os::HttpResponse> {
            if (offline) {
                if (error != nullptr) *error = "offline";
                return std::nullopt;
            }
            hub::os::HttpResponse response;
            if (request.url == LATEST_RELEASE_API_URL) {
                ++api_calls;
                response.status = api_status;
                response.body.assign(api_body.begin(), api_body.end());
            } else if (request.url == kDownloadUrl) {
                ++download_calls;
                response.status = download_status;
                response.body = archive;
                if (request.on_progress) {
                    request.on_progress(archive.size(), archive.size());
                    progress_reported = true;
                }
            } else {
                response.status = 404;
            }
            return response;
        };
    }
};

/// An install folder holding the build being replaced.
void write_old_build(const fs::path& dir) {
    write_file(dir / "ffxiv-hub.exe", "old exe");
    write_file(dir / "hub_payload.dll", "old dll");
}

std::string backup_name(std::string_view file) {
    return std::string(file) + "." + CURRENT_VERSION.to_string() + ".old";
}

} // namespace

TEST_CASE(Update, ParsesVersions) {
    TEST_ASSERT_TRUE(parse_version("1.2.3") == (Version{1, 2, 3}));
    TEST_ASSERT_TRUE(parse_version("v1.2.3") == (Version{1, 2, 3}));
    TEST_ASSERT_TRUE(parse_version("V10.0.65535") == (Version{10, 0, 65535}));
    for (const char* bad : {"", "v", "1.2", "1.2.3.4", "1.2.3-rc1", "1..3", "1.2.65536", "a.b.c", " 1.2.3"}) {
        TEST_ASSERT_FALSE(parse_version(bad).has_value());
    }
    TEST_ASSERT_TRUE((Version{1, 2, 3}) < (Version{1, 10, 0}));
    TEST_ASSERT_TRUE((Version{2, 0, 0}) > (Version{1, 99, 99}));
    TEST_ASSERT_EQ((Version{1, 2, 3}).to_string(), std::string("1.2.3"));
}

TEST_CASE(Update, ReadsTheLatestRelease) {
    const std::string digest = "sha256:" + std::string(64, 'A');
    const auto release = parse_latest_release(release_json(kNext, digest));
    TEST_ASSERT_TRUE(release.has_value());
    TEST_ASSERT_TRUE(release->version == kNext);
    TEST_ASSERT_EQ(release->page_url, kPageUrl);
    TEST_ASSERT_EQ(release->download_url, kDownloadUrl);
    TEST_ASSERT_EQ(release->download_size, uint64_t{2130658});
    // Lowercased, to compare with to_hex().
    TEST_ASSERT_EQ(release->sha256, std::string(64, 'a'));
    TEST_ASSERT_TRUE(release->installable());
}

TEST_CASE(Update, UntrustedFieldsAreLeftEmpty) {
    const std::string digest = "sha256:" + std::string(64, 'a');

    const auto elsewhere = parse_latest_release(
        release_json(kNext, digest, "https://example.com/ffxiv-hub-windows-x64.zip"));
    TEST_ASSERT_TRUE(elsewhere.has_value());
    TEST_ASSERT_FALSE(elsewhere->installable());
    // Still announced.
    TEST_ASSERT_TRUE(elsewhere->version == kNext);

    const auto other_repo = parse_latest_release(release_json(
        kNext, digest, "https://github.com/someone/ffxiv-hub/releases/download/v9.9.9/ffxiv-hub-windows-x64.zip"));
    TEST_ASSERT_FALSE(other_repo->installable());

    const auto http = parse_latest_release(release_json(
        kNext, digest, "http://github.com/mgauna-ar/ffxiv-hub/releases/download/v9.9.9/ffxiv-hub-windows-x64.zip"));
    TEST_ASSERT_FALSE(http->installable());

    for (const std::string& bad_digest : {std::string(), std::string("sha1:") + std::string(40, 'a'),
                                          std::string("sha256:") + std::string(63, 'a'),
                                          std::string("sha256:") + std::string(64, 'g')}) {
        const auto release = parse_latest_release(release_json(kNext, bad_digest));
        TEST_ASSERT_TRUE(release.has_value());
        TEST_ASSERT_FALSE(release->installable());
    }

    const auto page = parse_latest_release(release_json(kNext, digest, kDownloadUrl, "https://example.com/notes"));
    TEST_ASSERT_TRUE(page->page_url.empty());
}

TEST_CASE(Update, RejectsWhatIsNotARelease) {
    TEST_ASSERT_FALSE(parse_latest_release("").has_value());
    TEST_ASSERT_FALSE(parse_latest_release("not json").has_value());
    TEST_ASSERT_FALSE(parse_latest_release("[]").has_value());
    TEST_ASSERT_FALSE(parse_latest_release(R"({"message":"Not Found"})").has_value());
    TEST_ASSERT_FALSE(parse_latest_release(R"({"tag_name":"nightly"})").has_value());
    TEST_ASSERT_FALSE(parse_latest_release(R"({"tag_name":"v9.0.0","prerelease":true})").has_value());
    TEST_ASSERT_FALSE(parse_latest_release(R"({"tag_name":"v9.0.0","draft":true})").has_value());
    // A release without assets is still a release.
    TEST_ASSERT_TRUE(parse_latest_release(R"({"tag_name":"v9.0.0"})").has_value());
}

TEST_CASE(Update, StartupCheckRunsOnceAfterTheDelay) {
    FakeGitHub github;
    github.api_body = release_json(CURRENT_VERSION, sha256_digest(kZipPlain));
    Updater updater({}, github.fetch());

    const auto t0 = std::chrono::steady_clock::now();
    updater.tick(t0, true);
    updater.tick(t0 + Updater::kStartupCheckDelay - std::chrono::milliseconds(1), true);
    updater.wait_idle();
    TEST_ASSERT_EQ(github.api_calls.load(), 0);
    TEST_ASSERT_EQ(updater.state(), UpdateState::Idle);

    updater.tick(t0 + Updater::kStartupCheckDelay, true);
    updater.wait_idle();
    TEST_ASSERT_EQ(github.api_calls.load(), 1);
    TEST_ASSERT_EQ(updater.state(), UpdateState::UpToDate);

    updater.tick(t0 + std::chrono::hours(24), true);
    updater.wait_idle();
    TEST_ASSERT_EQ(github.api_calls.load(), 1);
}

TEST_CASE(Update, StartupCheckSkippedWhenSwitchedOff) {
    FakeGitHub github;
    github.api_body = release_json(kNext, sha256_digest(kZipPlain));
    Updater updater({}, github.fetch());

    const auto t0 = std::chrono::steady_clock::now();
    updater.tick(t0, false);
    updater.tick(t0 + Updater::kStartupCheckDelay, false);
    // Switched on later in the session: the startup check has passed.
    updater.tick(t0 + Updater::kStartupCheckDelay * 2, true);
    updater.wait_idle();
    TEST_ASSERT_EQ(github.api_calls.load(), 0);
    TEST_ASSERT_EQ(updater.state(), UpdateState::Idle);

    // "Check now" ignores the switch.
    updater.check_now();
    updater.wait_idle();
    TEST_ASSERT_EQ(github.api_calls.load(), 1);
    TEST_ASSERT_EQ(updater.state(), UpdateState::Available);
}

TEST_CASE(Update, OnlyANewerVersionIsAvailable) {
    for (const Version& latest : {CURRENT_VERSION, Version{0, 9, 9}}) {
        FakeGitHub github;
        github.api_body = release_json(latest, sha256_digest(kZipPlain));
        Updater updater({}, github.fetch());
        updater.check_now();
        updater.wait_idle();
        TEST_ASSERT_EQ(updater.state(), UpdateState::UpToDate);
        TEST_ASSERT_FALSE(updater.status().release.has_value());
    }

    FakeGitHub github;
    github.offer(kZipPlain);
    Updater updater({}, github.fetch());
    updater.check_now();
    updater.wait_idle();
    const UpdateStatus status = updater.status();
    TEST_ASSERT_EQ(status.state, UpdateState::Available);
    TEST_ASSERT_TRUE(status.release.has_value());
    TEST_ASSERT_TRUE(status.release->version == kNext);
    TEST_ASSERT_TRUE(status.can_install());
}

TEST_CASE(Update, CheckFailures) {
    {
        FakeGitHub github;
        github.offline = true;
        Updater updater({}, github.fetch());
        updater.check_now();
        updater.wait_idle();
        TEST_ASSERT_EQ(updater.state(), UpdateState::Failed);
        TEST_ASSERT_FALSE(updater.status().error.empty());
    }
    for (const int status : {403, 429, 500}) {
        FakeGitHub github;
        github.api_status = status;
        Updater updater({}, github.fetch());
        updater.check_now();
        updater.wait_idle();
        TEST_ASSERT_EQ(updater.state(), UpdateState::Failed);
    }
    {
        FakeGitHub github;
        github.api_body = "<html>";
        Updater updater({}, github.fetch());
        updater.check_now();
        updater.wait_idle();
        TEST_ASSERT_EQ(updater.state(), UpdateState::Failed);
    }
    {
        // No release published yet.
        FakeGitHub github;
        github.api_status = 404;
        Updater updater({}, github.fetch());
        updater.check_now();
        updater.wait_idle();
        TEST_ASSERT_EQ(updater.state(), UpdateState::UpToDate);
    }
}

TEST_CASE(Update, InstallIgnoredWithoutAnInstallableRelease) {
    FakeGitHub github;
    github.api_body = release_json(kNext, "");
    Updater updater({}, github.fetch());
    updater.install();
    updater.wait_idle();
    TEST_ASSERT_EQ(updater.state(), UpdateState::Idle);

    updater.check_now();
    updater.wait_idle();
    TEST_ASSERT_EQ(updater.state(), UpdateState::Available);
    TEST_ASSERT_FALSE(updater.status().can_install());
    updater.install();
    updater.wait_idle();
    TEST_ASSERT_EQ(updater.state(), UpdateState::Available);
    TEST_ASSERT_EQ(github.download_calls.load(), 0);
}

TEST_CASE(Update, InstallsAndAppliesARelease) {
    const ScratchDir install("hub_update_install");
    write_old_build(install.path);
    FakeGitHub github;
    github.offer(kZipPlain);
    Updater updater(install.path, github.fetch());

    updater.check_now();
    updater.wait_idle();
    updater.install();
    updater.wait_idle();
    TEST_ASSERT_EQ(updater.state(), UpdateState::Ready);
    TEST_ASSERT_TRUE(github.progress_reported.load());
    TEST_ASSERT_TRUE(updater.status().progress > 0.99f);

    // Staged beside the build, which is untouched until applied.
    const fs::path staged = staging_dir(install.path);
    TEST_ASSERT_EQ(read_file(staged / "ffxiv-hub.exe"), expected_exe());
    TEST_ASSERT_EQ(read_file(staged / "hub_payload.dll"), expected_dll());
    TEST_ASSERT_FALSE(fs::exists(staged / "notes.txt"));
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), std::string("old exe"));

    TEST_ASSERT_TRUE(updater.apply_staged());
    TEST_ASSERT_EQ(updater.state(), UpdateState::RestartPending);
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), expected_exe());
    TEST_ASSERT_EQ(read_file(install.path / "hub_payload.dll"), expected_dll());
    TEST_ASSERT_EQ(read_file(install.path / backup_name("ffxiv-hub.exe")), std::string("old exe"));
    TEST_ASSERT_EQ(read_file(install.path / backup_name("hub_payload.dll")), std::string("old dll"));
    TEST_ASSERT_FALSE(fs::exists(staged));

    // A second apply has nothing to apply.
    TEST_ASSERT_FALSE(updater.apply_staged());

    remove_update_leftovers(install.path);
    TEST_ASSERT_FALSE(fs::exists(install.path / backup_name("ffxiv-hub.exe")));
    TEST_ASSERT_FALSE(fs::exists(install.path / backup_name("hub_payload.dll")));
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), expected_exe());
}

TEST_CASE(Update, ArchiveWithDataDescriptorsInstalls) {
    const ScratchDir install("hub_update_descriptors");
    write_old_build(install.path);
    FakeGitHub github;
    github.offer(kZipWithDescriptors);
    Updater updater(install.path, github.fetch());
    updater.check_now();
    updater.wait_idle();
    updater.install();
    updater.wait_idle();
    TEST_ASSERT_EQ(updater.state(), UpdateState::Ready);
    TEST_ASSERT_EQ(read_file(staging_dir(install.path) / "hub_payload.dll"), expected_dll());
}

TEST_CASE(Update, ChecksumMismatchStagesNothing) {
    const ScratchDir install("hub_update_checksum");
    write_old_build(install.path);
    FakeGitHub github;
    github.offer(kZipPlain, sha256_digest(kZipWithDescriptors));
    Updater updater(install.path, github.fetch());
    updater.check_now();
    updater.wait_idle();
    updater.install();
    updater.wait_idle();

    const UpdateStatus status = updater.status();
    TEST_ASSERT_EQ(status.state, UpdateState::Failed);
    TEST_ASSERT_TRUE(status.error.find("checksum") != std::string::npos);
    TEST_ASSERT_FALSE(fs::exists(staging_dir(install.path)));
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), std::string("old exe"));
    // The release is kept, so it can be tried again.
    TEST_ASSERT_TRUE(status.can_install());
    TEST_ASSERT_FALSE(updater.apply_staged());
}

TEST_CASE(Update, ArchiveMissingAFileFails) {
    const ScratchDir install("hub_update_missing");
    write_old_build(install.path);
    FakeGitHub github;
    github.offer(kZipExeOnly);
    Updater updater(install.path, github.fetch());
    updater.check_now();
    updater.wait_idle();
    updater.install();
    updater.wait_idle();

    const UpdateStatus status = updater.status();
    TEST_ASSERT_EQ(status.state, UpdateState::Failed);
    TEST_ASSERT_TRUE(status.error.find("hub_payload.dll") != std::string::npos);
    TEST_ASSERT_FALSE(fs::exists(staging_dir(install.path)));
}

TEST_CASE(Update, DownloadFailureFails) {
    const ScratchDir install("hub_update_download");
    write_old_build(install.path);
    FakeGitHub github;
    github.offer(kZipPlain);
    github.download_status = 503;
    Updater updater(install.path, github.fetch());
    updater.check_now();
    updater.wait_idle();
    updater.install();
    updater.wait_idle();
    TEST_ASSERT_EQ(updater.state(), UpdateState::Failed);
    TEST_ASSERT_FALSE(fs::exists(staging_dir(install.path)));
}

TEST_CASE(Update, ApplyRollsBackOnAnyFailedStep) {
    // The renames, in order: exe aside, new exe in, dll aside, new dll in.
    for (int failing = 1; failing <= 4; ++failing) {
        const ScratchDir install("hub_update_rollback");
        write_old_build(install.path);
        const fs::path staged = staging_dir(install.path);
        fs::create_directories(staged);
        write_file(staged / "ffxiv-hub.exe", "new exe");
        write_file(staged / "hub_payload.dll", "new dll");

        int calls = 0;
        const RenameFn flaky_rename = [&](const fs::path& from, const fs::path& to) {
            if (++calls == failing) return false;
            std::error_code ec;
            fs::rename(from, to, ec);
            return !ec;
        };
        const std::string error = apply_staged_update(install.path, staged, "1.0.0", flaky_rename);
        TEST_ASSERT_FALSE(error.empty());
        TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), std::string("old exe"));
        TEST_ASSERT_EQ(read_file(install.path / "hub_payload.dll"), std::string("old dll"));
        TEST_ASSERT_EQ(read_file(staged / "ffxiv-hub.exe"), std::string("new exe"));
        TEST_ASSERT_EQ(read_file(staged / "hub_payload.dll"), std::string("new dll"));
        TEST_ASSERT_FALSE(fs::exists(install.path / "ffxiv-hub.exe.1.0.0.old"));
    }
}

TEST_CASE(Update, ApplyNeedsBothStagedFiles) {
    const ScratchDir install("hub_update_partial");
    write_old_build(install.path);
    const fs::path staged = staging_dir(install.path);
    fs::create_directories(staged);
    write_file(staged / "ffxiv-hub.exe", "new exe");

    TEST_ASSERT_FALSE(apply_staged_update(install.path, staged, "1.0.0").empty());
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), std::string("old exe"));
    TEST_ASSERT_EQ(read_file(staged / "ffxiv-hub.exe"), std::string("new exe"));
}

TEST_CASE(Update, ApplyNumbersABackupItCannotReplace) {
    // A backup from an earlier update that could not be removed, as a DLL the
    // game still has mapped; a non-empty folder stands in for it here.
    const ScratchDir install("hub_update_numbered");
    write_old_build(install.path);
    fs::create_directories(install.path / "hub_payload.dll.1.0.0.old");
    write_file(install.path / "hub_payload.dll.1.0.0.old" / "held", "x");
    const fs::path staged = staging_dir(install.path);
    fs::create_directories(staged);
    write_file(staged / "ffxiv-hub.exe", "new exe");
    write_file(staged / "hub_payload.dll", "new dll");

    TEST_ASSERT_TRUE(apply_staged_update(install.path, staged, "1.0.0").empty());
    TEST_ASSERT_EQ(read_file(install.path / "hub_payload.dll"), std::string("new dll"));
    TEST_ASSERT_EQ(read_file(install.path / "hub_payload.dll.1.0.0.2.old"), std::string("old dll"));
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe.1.0.0.old"), std::string("old exe"));
}

TEST_CASE(Update, LeftoverCleanupTouchesOnlyBackups) {
    const ScratchDir install("hub_update_leftovers");
    write_old_build(install.path);
    write_file(install.path / "ffxiv-hub.exe.1.0.0.old", "a");
    write_file(install.path / "hub_payload.dll.1.0.0.2.old", "b");
    write_file(install.path / "other.dll.1.0.0.old", "c");
    write_file(install.path / "ffxiv-hub.exe.old.txt", "d");
    write_file(install.path / "config.json", "{}");
    fs::create_directories(staging_dir(install.path));
    write_file(staging_dir(install.path) / "ffxiv-hub.exe", "e");

    remove_update_leftovers(install.path);
    TEST_ASSERT_FALSE(fs::exists(install.path / "ffxiv-hub.exe.1.0.0.old"));
    TEST_ASSERT_FALSE(fs::exists(install.path / "hub_payload.dll.1.0.0.2.old"));
    TEST_ASSERT_FALSE(fs::exists(staging_dir(install.path)));
    TEST_ASSERT_TRUE(fs::exists(install.path / "other.dll.1.0.0.old"));
    TEST_ASSERT_TRUE(fs::exists(install.path / "ffxiv-hub.exe.old.txt"));
    TEST_ASSERT_TRUE(fs::exists(install.path / "config.json"));
    TEST_ASSERT_EQ(read_file(install.path / "ffxiv-hub.exe"), std::string("old exe"));

    // An unknown folder is no error.
    remove_update_leftovers({});
}

TEST_CASE(Update, NotifierBalloonsOncePerVersion) {
    UpdateNotifier notifier;
    const auto first = notifier.update(kNext);
    TEST_ASSERT_TRUE(first.has_value());
    TEST_ASSERT_TRUE(first->message.find(kNext.to_string()) != std::string::npos);
    TEST_ASSERT_FALSE(notifier.update(kNext).has_value());

    const Version later{kNext.major, static_cast<uint16_t>(kNext.minor + 1), 0};
    TEST_ASSERT_TRUE(notifier.update(later).has_value());
    TEST_ASSERT_FALSE(notifier.update(later).has_value());
}
