#include "dlsvc/api/http_api.h"

#include <httplib.h>
#include <thread>

#include "dlsvc/log.h"
#include "dlsvc/mini_json.h"
#include "dlsvc/util.h"

namespace dlsvc {

struct HttpApi::Impl {
    Impl(TaskManager& t, const ServiceConfig& c) : tm(t), cfg(c) {}

    TaskManager& tm;
    ServiceConfig cfg;
    httplib::Server svr;
    std::thread th;

    static void sendJson(httplib::Response& res, int status, const std::string& body) {
        res.status = status;
        res.set_content(body, "application/json");
    }

    static void sendErr(httplib::Response& res, int status, const std::string& msg) {
        mini::Json j = mini::Json::object();
        j["error"] = mini::Json(msg);
        sendJson(res, status, j.dump());
    }

    static mini::Json taskToJson(const TaskRow& row, const std::vector<SegmentRow>& segs) {
        mini::Json j = mini::Json::object();
        j["id"] = (long long)row.id;
        j["user_id"] = row.userId;
        j["url"] = row.url;
        j["save_path"] = row.savePath;
        j["status"] = std::string(toString(row.status));
        j["total_size"] = (long long)row.totalSize;
        j["downloaded_bytes"] = (long long)row.downloadedBytes;
        if (row.totalSize > 0)
            j["progress_percent"] = 100.0 * (double)row.downloadedBytes / (double)row.totalSize;
        j["fail_reason"] = row.failReason;
        j["retry_count"] = (long long)row.retryCount;
        j["created_at"] = row.createdAt;
        j["updated_at"] = row.updatedAt;
        mini::Json arr = mini::Json::array();
        for (const auto& s : segs) {
            mini::Json sv = mini::Json::object();
            sv["seg_index"] = (long long)s.segIndex;
            sv["start"] = (long long)s.segStart;
            sv["end"] = (long long)s.segEnd;
            sv["downloaded"] = (long long)s.downloaded;
            sv["finished"] = s.finished;
            arr.push_back(std::move(sv));
        }
        j["segments"] = std::move(arr);
        return j;
    }

    void setup() {
        svr.set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                      std::exception_ptr ep) {
            std::string msg = "unknown exception";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                msg = e.what();
            }
            LOG_ERROR("api exception: %s", msg.c_str());
            sendErr(res, 500, msg);
        });

        svr.Get("/healthz", [this](const httplib::Request&, httplib::Response& res) {
            long long active = 0;
            size_t q = 0, w = 0;
            tm.healthSnapshot(active, q, w);
            mini::Json j = mini::Json::object();
            j["ok"] = mini::Json(true);
            j["active_tasks"] = (long long)active;
            j["queue_size"] = (long long)q;
            j["workers"] = (long long)w;
            sendJson(res, 200, j.dump());
        });

        svr.Post("/api/task/submit", [this](const httplib::Request& req, httplib::Response& res) {
            handleSubmit(req, res);
        });
        svr.Get(R"(/api/task/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            handleGet(std::stoll(req.matches[1].str()), res);
        });
        svr.Post(R"(/api/task/(\d+)/cancel)",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handleCancel(std::stoll(req.matches[1].str()), res);
                 });
        svr.Get("/api/tasks", [this](const httplib::Request& req, httplib::Response& res) {
            handleList(req, res);
        });
    }

    // POST /api/task/submit   body: {"url":"...","save_path":"...","user_id":"..."(可选)}
    void handleSubmit(const httplib::Request& req, httplib::Response& res) {
        mini::Json body;
        std::string err;
        if (!mini::Json::parse(req.body, body, err))
            return sendErr(res, 400, "bad json: " + err);

        const mini::Json* url = body.find("url");
        const mini::Json* sp = body.find("save_path");
        if (!url || !url->isString() || url->asString().empty())
            return sendErr(res, 400, "url required");
        if (!sp || !sp->isString() || sp->asString().empty())
            return sendErr(res, 400, "save_path required");

        std::string userId = "default";
        if (const mini::Json* u = body.find("user_id")) {
            if (u->isString() && !u->asString().empty()) userId = u->asString();
        }
        if (userId.size() > 64) return sendErr(res, 400, "user_id too long");

        long long id = 0;
        bool resumed = false;
        int status = 0;
        std::string err2;
        if (!tm.submit(userId, url->asString(), sp->asString(), id, resumed, status, err2))
            return sendErr(res, status, err2);

        mini::Json j = mini::Json::object();
        j["task_id"] = (long long)id;
        j["resumed"] = resumed;
        sendJson(res, status, j.dump());
    }

    // GET /api/task/{id}
    void handleGet(long long id, httplib::Response& res) {
        TaskRow row;
        std::vector<SegmentRow> segs;
        if (!tm.getTask(id, row, segs)) return sendErr(res, 404, "task not found");
        sendJson(res, 200, taskToJson(row, segs).dump());
    }

    // POST /api/task/{id}/cancel
    void handleCancel(long long id, httplib::Response& res) {
        int status = 0;
        std::string err;
        if (!tm.cancel(id, status, err)) return sendErr(res, status, err);
        mini::Json j = mini::Json::object();
        j["ok"] = mini::Json(true);
        j["status"] = std::string("canceling"); // 终态由 worker 协作退出后落库
        sendJson(res, 200, j.dump());
    }

    // GET /api/tasks?status=&user_id=&limit=
    void handleList(const httplib::Request& req, httplib::Response& res) {
        std::string status = req.has_param("status") ? req.get_param_value("status") : "";
        std::string userId = req.has_param("user_id") ? req.get_param_value("user_id") : "";
        int limit = 50;
        if (req.has_param("limit")) {
            long long n = 0;
            if (util::parseLL(req.get_param_value("limit"), n)) limit = (int)n;
        }
        std::vector<TaskRow> rows;
        if (!tm.listTasks(status, userId, limit, rows)) return sendErr(res, 500, "db error");
        mini::Json arr = mini::Json::array();
        for (const auto& row : rows) arr.push_back(taskToJson(row, {}));
        mini::Json j = mini::Json::object();
        j["count"] = (long long)rows.size();
        j["tasks"] = std::move(arr);
        sendJson(res, 200, j.dump());
    }
};

HttpApi::HttpApi(TaskManager& tm, const ServiceConfig& cfg) : impl_(new Impl{tm, cfg}) {
    impl_->setup();
}

HttpApi::~HttpApi() { stop(); }

bool HttpApi::start(std::string* err) {
    impl_->th = std::thread([this] {
        impl_->svr.listen(impl_->cfg.listenHost.c_str(), impl_->cfg.listenPort);
    });
    impl_->svr.wait_until_ready();
    LOG_INFO("http api listening on %s:%d", impl_->cfg.listenHost.c_str(), impl_->cfg.listenPort);
    return true;
}

void HttpApi::stop() {
    impl_->svr.stop();
    if (impl_->th.joinable()) impl_->th.join();
}

} // namespace dlsvc
