#!/usr/bin/env python3
# 测试用 Range-HTTP 文件服务器（python http.server 不支持 Range，下载器测不了分片）
#
# 用法: python3 scripts/range_server.py [port] [directory]
# 环境变量:
#   DL_RATE  限速字节/秒（0 = 不限），冒烟脚本用它把下载拖慢到可控时长

import http.server
import os
import re
import socketserver
import sys
import time

RATE = int(os.environ.get("DL_RATE", "0"))  # bytes/s
CHUNK = 65536


class RangeHandler(http.server.SimpleHTTPRequestHandler):

    # 下载器取消/断开会主动复位连接，往已关闭的 socket 写数据触发
    # ConnectionResetError 是预期行为——静默，不打扰测试输出
    def handle_error(self, request, client_address):
        pass

    def send_head(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        f = open(path, "rb")
        self._limit = None

        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng)
            if not m:
                f.close()
                self.send_error(400, "bad range")
                return None
            start = int(m.group(1) or 0)
            end = int(m.group(2)) if m.group(2) else size - 1
            end = min(end, size - 1)
            if start > end or start >= size:
                f.close()
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.end_headers()
                return None
            self.send_response(206)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.send_header("Content-Length", str(end - start + 1))
            self.end_headers()
            f.seek(start)
            self._limit = end - start + 1
            return f

        self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        return f

    def copyfile(self, src, dst):
        limit = getattr(self, "_limit", None)
        sent = 0
        while True:
            n = CHUNK
            if limit is not None:
                n = min(CHUNK, limit - sent)
                if n <= 0:
                    break
            b = src.read(n)
            if not b:
                break
            dst.write(b)
            sent += len(b)
            if RATE:
                time.sleep(CHUNK / RATE)

    def log_message(self, fmt, *args):  # 静默：冒烟脚本自己断言
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    directory = sys.argv[2] if len(sys.argv) > 2 else "."
    os.chdir(directory)
    socketserver.TCPServer.allow_reuse_address = True
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), RangeHandler)
    print("range server on :%d serving %s rate=%d" % (port, directory, RATE), flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
