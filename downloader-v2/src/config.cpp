#include "dlsvc/config.h"

#include <fstream>

#include "dlsvc/log.h"
#include "dlsvc/util.h"

namespace dlsvc {

bool ServiceConfig::loadFromFile(const std::string& path, std::string* err) {
    std::ifstream in(path);
    if (!in) {
        if (err) *err = "cannot open config file: " + path;
        return false;
    }

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = util::trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (err) *err = util::stringPrintf("%s:%d: expect key=value", path.c_str(), lineNo);
            return false;
        }
        std::string key = util::toLower(util::trim(line.substr(0, eq)));
        std::string val = util::trim(line.substr(eq + 1));

        long long n = 0;
        bool b = false;
        if (key == "listen_host") listenHost = val;
        else if (key == "listen_port") { if (!util::parseLL(val, n)) goto badval; listenPort = (int)n; }
        else if (key == "storage_root") storageRoot = val;
        else if (key == "restrict_to_root") { if (!util::parseBool(val, b)) goto badval; restrictToRoot = b; }
        else if (key == "worker_count") { if (!util::parseLL(val, n)) goto badval; workerCount = (int)n; }
        else if (key == "queue_capacity") { if (!util::parseLL(val, n)) goto badval; queueCapacity = (int)n; }
        else if (key == "max_active_per_user") { if (!util::parseLL(val, n)) goto badval; maxActivePerUser = (int)n; }
        else if (key == "flush_interval_ms") { if (!util::parseLL(val, n)) goto badval; flushIntervalMs = n; }
        else if (key == "flush_bytes_mb") { if (!util::parseLL(val, n)) goto badval; flushBytes = n * 1024 * 1024; }
        else if (key == "max_segments_per_task") { if (!util::parseLL(val, n)) goto badval; download.maxSegmentsPerTask = (int)n; }
        else if (key == "min_segment_size_mb") { if (!util::parseLL(val, n)) goto badval; download.minSegmentSize = n * 1024 * 1024; }
        else if (key == "max_retries") { if (!util::parseLL(val, n)) goto badval; download.maxRetries = n; }
        else if (key == "retry_backoff_ms") { if (!util::parseLL(val, n)) goto badval; download.retryBackoffMs = n; }
        else if (key == "curl_connect_timeout_s") { if (!util::parseLL(val, n)) goto badval; download.connectTimeoutS = (long)n; }
        else if (key == "stall_speed_bps") { if (!util::parseLL(val, n)) goto badval; download.stallSpeedBps = (long)n; }
        else if (key == "stall_time_s") { if (!util::parseLL(val, n)) goto badval; download.stallTimeS = (long)n; }
        else if (key == "mysql_host") db.host = val;
        else if (key == "mysql_port") { if (!util::parseLL(val, n)) goto badval; db.port = (int)n; }
        else if (key == "mysql_user") db.user = val;
        else if (key == "mysql_password") db.password = val;
        else if (key == "mysql_db") db.db = val;
        else if (key == "log_level") logLevel = val;
        else if (key == "log_file") logFile = val;
        else fprintf(stderr, "[config] %s:%d: unknown key '%s' ignored\n", path.c_str(), lineNo, key.c_str());
        continue;
    badval:
        if (err) *err = util::stringPrintf("%s:%d: bad value for '%s'", path.c_str(), lineNo, key.c_str());
        return false;
    }
    return true;
}

} // namespace dlsvc
