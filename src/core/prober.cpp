#include "mtdl/core/prober.hpp"

#include <cstdint>

#include "mtdl/net/http/response.hpp"
#include "mtdl/util/error.hpp"
#include "mtdl/util/log.hpp"
#include "mtdl/util/strings.hpp"

namespace mtdl {

ProbeResult Prober::probe() {
  ProbeResult out;
  HttpClient client(url_, opts_);
  client.connect();
  // bytes=0-0: one byte of body, full size in Content-Range.
  client.request_range(0, 0);

  const ResponseHead& head = client.head();
  const int status = head.status();

  uint64_t first = 0, last = 0, total = 0;
  if (status == 206 && head.content_range(&first, &last, &total)) {
    out.size = total;
    out.range_supported = true;
  } else if (status == 200) {
    bool ok = false;
    out.size = head.content_length(&ok);
    out.range_supported = false;
    LOG_WARN("server ignored the Range header; falling back to a single connection");
  } else if (status == 416) {
    fail(ErrKind::Protocol, "server rejected the probe range (416)");
  } else if (status == 403) {
    fail(ErrKind::Protocol,
         "HTTP 403 Forbidden - the mirror refused this client (User-Agent ACL); "
         "check the URL or try --plain-http");
  } else if (status >= 300 && status < 400) {
    fail(ErrKind::Protocol,
         "unexpected redirect (HTTP " + std::to_string(status) +
             "); pass the final URL directly (following redirects is not implemented)");
  } else {
    fail(ErrKind::Protocol, "probe failed: HTTP " + std::to_string(status) + " " + head.reason());
  }

  if (out.size == 0) fail(ErrKind::Protocol, "server reported a zero-length object");

  out.etag = head.header("etag");
  out.last_modified = head.header("last-modified");
  out.content_type = head.header("content-type");

  LOG_INFO("probe: " << url_.str());
  LOG_INFO("size=" << str::format_bytes(out.size) << " (" << out.size << " bytes), ranges="
                   << (out.range_supported ? "yes" : "no") << ", type=" << out.content_type
                   << ", etag=" << out.etag);
  return out;
}

}  // namespace mtdl
