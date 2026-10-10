#include "app/updater.hpp"
#include "common/archive/zip_reader.hpp"
#include "common/os/logger.hpp"
#include "common/os/paths.hpp"
#include "common/sha256.hpp"

#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace hub::app {

namespace fs = std::filesystem;

namespace {

bool rename_file(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
}

/// "<name>.<tag>.old", or a numbered one when that is taken by a backup that
/// could not be removed, such as a DLL the game still has mapped.
fs::path free_backup_path(const fs::path& install_dir, std::string_view name, std::string_view tag) {
    const std::string stem = std::string(name) + "." + std::string(tag);
    std::error_code ec;
    fs::path backup = install_dir / (stem + ".old");
    fs::remove(backup, ec);
    for (int n = 2; fs::exists(backup, ec) && n < 100; ++n) {
        backup = install_dir / (stem + "." + std::to_string(n) + ".old");
    }
    return backup;
}

bool is_backup_name(std::string_view file) {
    if (!file.ends_with(".old")) return false;
    for (const std::string_view name : UPDATE_FILES) {
        if (file.size() > name.size() + 1 && file.starts_with(name) && file[name.size()] == '.') return true;
    }
    return false;
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return out.good();
}

} // namespace

fs::path staging_dir(const fs::path& install_dir) {
    return install_dir / ".ffxiv-hub-update";
}

std::string apply_staged_update(const fs::path& install_dir, const fs::path& staging,
                                std::string_view backup_tag, const RenameFn& rename_fn) {
    const RenameFn do_rename = rename_fn ? rename_fn : RenameFn(rename_file);
    std::error_code ec;
    for (const std::string_view name : UPDATE_FILES) {
        if (!fs::is_regular_file(staging / name, ec)) {
            return "The downloaded " + std::string(name) + " is missing.";
        }
    }

    struct Move {
        fs::path from;
        fs::path to;
    };
    std::vector<Move> done;
    const auto undo = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            do_rename(it->to, it->from);
        }
    };

    for (const std::string_view name : UPDATE_FILES) {
        const fs::path live = install_dir / name;
        if (fs::exists(live, ec)) {
            const fs::path backup = free_backup_path(install_dir, name, backup_tag);
            if (!do_rename(live, backup)) {
                undo();
                return "Couldn't move the current " + std::string(name) + " aside.";
            }
            done.push_back({live, backup});
        }
        if (!do_rename(staging / name, live)) {
            undo();
            return "Couldn't move the new " + std::string(name) + " into place.";
        }
        done.push_back({staging / name, live});
    }
    return {};
}

void remove_update_leftovers(const fs::path& install_dir) {
    if (install_dir.empty()) return;
    std::error_code ec;
    fs::remove_all(staging_dir(install_dir), ec);

    std::vector<fs::path> backups;
    fs::directory_iterator it(install_dir, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (is_backup_name(os::to_utf8(it->path().filename()))) backups.push_back(it->path());
    }
    for (const fs::path& backup : backups) {
        std::error_code remove_ec;
        if (fs::remove(backup, remove_ec)) {
            os::Logger::info("Removed " + os::to_utf8(backup.filename()) + " left by an update.");
        }
    }
}

Updater::Updater(fs::path folder, Fetch fetch)
    : m_install_dir(std::move(folder)), m_fetch(std::move(fetch)) {}

Updater::~Updater() {
    stop();
}

void Updater::tick(std::chrono::steady_clock::time_point now, bool auto_check) {
    if (m_startup_check_done) return;
    if (!m_first_tick) m_first_tick = now;
    if (now - *m_first_tick < kStartupCheckDelay) return;
    m_startup_check_done = true;
    if (auto_check) check_now();
}

void Updater::check_now() {
    // Once installed, only a restart is left to do.
    if (m_busy.load() || status().busy() || state() == UpdateState::RestartPending) return;
    set_state(UpdateState::Checking);
    start([this] { run_check(); });
}

void Updater::install() {
    std::optional<ReleaseInfo> release;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_busy.load() || !m_status.can_install()) return;
        release = m_status.release;
        m_status.progress = 0.0f;
        m_status.error.clear();
    }
    set_state(UpdateState::Downloading);
    start([this, target = *release] { run_install(target); });
}

bool Updater::apply_staged() {
    if (state() != UpdateState::Ready) return false;
    const std::string error = apply_staged_update(m_install_dir, staging_dir(m_install_dir),
                                                  CURRENT_VERSION.to_string());
    if (!error.empty()) {
        os::Logger::error("Update could not be installed: " + error);
        fail(error);
        return false;
    }
    std::error_code ec;
    fs::remove_all(staging_dir(m_install_dir), ec);
    os::Logger::info("Update installed into " + os::to_utf8(m_install_dir) + ".");
    set_state(UpdateState::RestartPending);
    return true;
}

UpdateStatus Updater::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void Updater::stop() {
    m_cancel.store(true);
    wait_idle();
}

void Updater::wait_idle() {
    if (m_worker.joinable()) m_worker.join();
}

void Updater::start(std::function<void()> job) {
    wait_idle();
    m_busy.store(true);
    m_worker = std::thread([this, work = std::move(job)] {
        work();
        m_busy.store(false);
    });
}

void Updater::set_state(UpdateState next) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.state = next;
    m_state.store(next);
}

void Updater::fail(std::string error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.error = std::move(error);
    m_status.state = UpdateState::Failed;
    m_state.store(UpdateState::Failed);
}

void Updater::run_check() {
    os::HttpRequest request;
    request.url = std::string(LATEST_RELEASE_API_URL);
    request.headers = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
    request.max_bytes = kMaxApiBytes;
    request.cancel = &m_cancel;

    std::string error;
    const auto response = m_fetch(request, &error);
    if (!response) {
        os::Logger::warn("Update check failed: " + error);
        return fail("Couldn't reach GitHub.");
    }
    if (response->status == 404) {
        // No release published yet.
        os::Logger::info("Update check: no release published.");
        return set_state(UpdateState::UpToDate);
    }
    if (response->status == 403 || response->status == 429) {
        os::Logger::warn("Update check: GitHub is rate limiting (HTTP " + std::to_string(response->status) + ").");
        return fail("GitHub is limiting requests. Try again later.");
    }
    if (response->status != 200) {
        os::Logger::warn("Update check: GitHub answered HTTP " + std::to_string(response->status) + ".");
        return fail("GitHub answered with HTTP " + std::to_string(response->status) + ".");
    }

    const std::string_view body(reinterpret_cast<const char*>(response->body.data()), response->body.size());
    const auto release = parse_latest_release(body);
    if (!release) {
        os::Logger::warn("Update check: GitHub's reply is not a release.");
        return fail("GitHub's reply could not be read.");
    }
    if (release->version <= CURRENT_VERSION) {
        os::Logger::info("Update check: up to date (latest release " + release->version.to_string() + ").");
        return set_state(UpdateState::UpToDate);
    }

    os::Logger::info("Update check: version " + release->version.to_string() + " is available" +
                     (release->installable() ? "." : ", but has no verifiable download."));
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.release = *release;
    m_status.error.clear();
    m_status.state = UpdateState::Available;
    m_state.store(UpdateState::Available);
}

void Updater::run_install(const ReleaseInfo& release) {
    os::Logger::info("Downloading version " + release.version.to_string() + "...");
    const std::string error = stage(release);
    if (!error.empty()) {
        std::error_code ec;
        fs::remove_all(staging_dir(m_install_dir), ec);
        os::Logger::error("Update to " + release.version.to_string() + " failed: " + error);
        return fail(error);
    }
    os::Logger::info("Version " + release.version.to_string() + " downloaded and verified.");
    set_state(UpdateState::Ready);
}

std::string Updater::stage(const ReleaseInfo& release) {
    if (m_install_dir.empty()) return "The Hub's own folder is unknown.";
    const fs::path dir = staging_dir(m_install_dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        return "Can't write to " + os::to_utf8(m_install_dir) +
               ". Run FFXIV Hub as administrator, or update by hand.";
    }

    os::HttpRequest request;
    request.url = release.download_url;
    request.max_bytes = kMaxDownloadBytes;
    request.cancel = &m_cancel;
    request.on_progress = [this, expected = release.download_size](size_t received, size_t total) {
        const size_t size = total != 0 ? total : static_cast<size_t>(expected);
        if (size == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.progress = static_cast<float>(static_cast<double>(received) / static_cast<double>(size));
    };
    std::string error;
    const auto response = m_fetch(request, &error);
    if (!response) {
        os::Logger::warn("Update download failed: " + error);
        return "The download failed.";
    }
    if (response->status != 200) return "The download failed (HTTP " + std::to_string(response->status) + ").";

    const auto digest = common::Sha256::hash(response->body);
    if (common::to_hex(digest) != release.sha256) return "The download didn't match its published checksum.";

    const auto zip = archive::ZipReader::open(response->body);
    if (!zip) return "The downloaded archive could not be read.";
    for (const std::string_view name : UPDATE_FILES) {
        const archive::ZipEntry* entry = zip->find(name);
        if (entry == nullptr) return "The release archive has no " + std::string(name) + ".";
        std::string why;
        const auto data = zip->extract(*entry, kMaxFileBytes, &why);
        if (!data) return "Couldn't extract " + std::string(name) + ": " + why + ".";
        if (!write_file(dir / name, *data)) return "Couldn't write " + os::to_utf8(dir / name) + ".";
    }
    return {};
}

} // namespace hub::app
