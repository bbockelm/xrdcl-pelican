/***************************************************************
 *
 * Copyright (C) 2025, Pelican Project, Morgridge Institute for Research
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "CurlOps.hh"
#include "CurlUtil.hh"
#include "CurlWorker.hh"
#include "PelicanFile.hh"

#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClLog.hh>
#include <XrdCl/XrdClXRootDResponses.hh>
#include <XrdOuc/XrdOucCRC.hh>
#include <XrdSys/XrdSysPageSize.hh>

#include <curl/curl.h>
#include <tinyxml2.h>

#include <unistd.h>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <iostream>
#include <vector>

using namespace Pelican;


CurlOperation::CurlOperation(XrdCl::ResponseHandler *handler, const std::string &url,
    struct timespec timeout, XrdCl::Log *logger) :
    m_header_timeout(timeout),
    m_url(url),
    m_handler(handler),
    m_curl(nullptr, &curl_easy_cleanup),
    m_logger(logger)
    {}

CurlOperation::~CurlOperation() {
    if (m_broker_reverse_socket != -1) {
        close(m_broker_reverse_socket);
    }
}

void
CurlOperation::Fail(uint16_t errCode, uint32_t errNum, const std::string &msg)
{
    SetDone();
    if (m_handler == nullptr) {return;}
    if (!msg.empty()) {
        m_logger->Debug(kLogXrdClPelican, "curl operation failed with message: %s", msg.c_str());
    } else {
        m_logger->Debug(kLogXrdClPelican, "curl operation failed with status code %d", errNum);
    }
    auto status = new XrdCl::XRootDStatus(XrdCl::stError, errCode, errNum, msg);
    m_handler->HandleResponse(status, nullptr);
    m_handler = nullptr;
}

size_t
CurlOperation::HeaderCallback(char *buffer, size_t size, size_t nitems, void *this_ptr)
{
    std::string header(buffer, size * nitems);
    auto me = static_cast<CurlOperation*>(this_ptr);
    me->m_received_header = true;
    auto rv = me->Header(header);
    return rv ? (size * nitems) : 0;
}

bool
CurlOperation::Header(const std::string &header)
{
    auto result = m_headers.Parse(header);
    // m_logger->Debug(kLogXrdClPelican, "Got header: %s", header.c_str());
    if (!result) {
        m_logger->Debug(kLogXrdClPelican, "Failed to parse response header: %s", header.c_str());
    }
    if (m_headers.HeadersDone() && HTTPStatusIsError(m_headers.GetStatusCode())) {
        auto httpErr = HTTPStatusConvert(m_headers.GetStatusCode());
        m_logger->Debug(kLogXrdClPelican, "Status code %d", m_headers.GetStatusCode());
        Fail(httpErr.first, httpErr.second, m_headers.GetStatusMessage());
    }
    return result;
}

bool
CurlOperation::Redirect()
{
    auto broker = m_headers.GetBroker();
    m_broker.reset();
    m_broker_reverse_socket = -1;
    auto location = m_headers.GetLocation();
    if (location.empty()) {
        m_logger->Warning(kLogXrdClPelican, "After request to %s, server returned a redirect with no new location", m_url.c_str());
        Fail(XrdCl::errErrorResponse, kXR_ServerError, "Server returned redirect without updated location");
        return false;
    }
    m_logger->Debug(kLogXrdClPelican, "Request for %s redirected to %s", m_url.c_str(), location.c_str());
    curl_easy_setopt(m_curl.get(), CURLOPT_URL, location.c_str());
    std::tie(m_mirror_url, m_mirror_depth) = m_headers.GetMirrorInfo();
    if (m_headers.GetX509Auth()) {
        m_x509_auth = true;
        auto env = XrdCl::DefaultEnv::GetEnv();
        std::string cert, key;
        m_logger->Debug(kLogXrdClPelican, "Will use client X509 auth for future operations");
        env->GetString("PelicanClientCertFile", cert);
        env->GetString("PelicanClientKeyFile", key);
        if (!cert.empty())
            curl_easy_setopt(m_curl.get(), CURLOPT_SSLCERT, cert.c_str());
        if (!key.empty())
            curl_easy_setopt(m_curl.get(), CURLOPT_SSLKEY, key.c_str());
    }
    m_headers = HeaderParser();
    if (!broker.empty()) {
        m_broker_url = broker;
        m_broker.reset(new BrokerRequest(m_curl.get(), broker));
        std::string err;
        if (m_broker->StartRequest(err) == -1) {
            auto errMsg = "Failed to start a read request for broker " + broker + ": " + err;
            Fail(XrdCl::errInternal, 1, errMsg.c_str());
            return false;
        }
        curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETFUNCTION, CurlReadOp::OpenSocketCallback);
        curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETDATA, this);
        curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTFUNCTION, CurlReadOp::SockOptCallback);
        curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTDATA, this);
    }
    return true;
}

namespace {

size_t
NullCallback(char * /*buffer*/, size_t size, size_t nitems, void * /*this_ptr*/)
{
    return size * nitems;
}

}

bool
CurlOperation::StartBroker(std::string &err)
{
    if (m_broker_url.empty()) {
        err = "Broker URL is not set";
        Fail(XrdCl::errInternal, 1, err.c_str());
        return false;
    }
    if (m_broker->StartRequest(err) == -1) {
        err = "Failed to start a read request for broker " + m_broker_url + ": " + err;
        Fail(XrdCl::errInternal, 1, err.c_str());
        return false;
    }
    return true;
}

bool
CurlOperation::HeaderTimeoutExpired() {
    if (m_received_header) return false;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return false;
    }

    auto res = (now.tv_sec > m_header_expiry.tv_sec || (now.tv_sec == m_header_expiry.tv_sec && now.tv_nsec > m_header_expiry.tv_nsec));
    if (res) {
        m_error = OpError::ErrHeaderTimeout;
    }
    return res;
}

void
CurlOperation::Setup(CURL *curl, CurlWorker &worker)
{
    if (curl == nullptr) {
        throw std::runtime_error("Unable to setup curl operation with no handle");
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        throw std::runtime_error("Unable to get current time");
    }
    if (m_header_timeout.tv_sec == 0 && m_header_timeout.tv_nsec == 0) {
        m_header_timeout = {30, 0};
    }
    m_header_expiry.tv_sec = now.tv_sec + m_header_timeout.tv_sec;
    m_header_expiry.tv_nsec = now.tv_nsec + m_header_timeout.tv_nsec;
    while (m_header_expiry.tv_nsec > 1'000'000'000) {
        m_header_expiry.tv_nsec -= 1'000'000'000;
        m_header_expiry.tv_sec ++;
    }

    m_curl.reset(curl);
    curl_easy_setopt(m_curl.get(), CURLOPT_URL, m_url.c_str());
    curl_easy_setopt(m_curl.get(), CURLOPT_HEADERFUNCTION, CurlStatOp::HeaderCallback);
    curl_easy_setopt(m_curl.get(), CURLOPT_HEADERDATA, this);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, NullCallback);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_XFERINFOFUNCTION, CurlOperation::XferInfoCallback);
    curl_easy_setopt(m_curl.get(), CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(m_curl.get(), CURLOPT_NOPROGRESS, 0L);

    m_parsed_url.reset(new XrdCl::URL(m_url));
    if (m_x509_auth || worker.UseX509Auth(*m_parsed_url)) {
        auto [cert, key] = worker.ClientX509CertKeyFile();
        curl_easy_setopt(m_curl.get(), CURLOPT_SSLCERT, cert.c_str());
        curl_easy_setopt(m_curl.get(), CURLOPT_SSLKEY, key.c_str());
    }

    if (!m_broker_url.empty()) {
        m_broker.reset(new BrokerRequest(m_curl.get(), m_broker_url));
        curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETFUNCTION, CurlReadOp::OpenSocketCallback);
        curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETDATA, this);
        curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTFUNCTION, CurlReadOp::SockOptCallback);
        curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTDATA, this);
    }
}

void
CurlOperation::ReleaseHandle()
{
    if (m_curl == nullptr) return;
    curl_easy_setopt(m_curl.get(), CURLOPT_SSLCERT, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_SSLKEY, nullptr);
    m_curl.release();
}

curl_socket_t
CurlOperation::OpenSocketCallback(void *clientp, curlsocktype purpose, struct curl_sockaddr *address)
{
    auto me = reinterpret_cast<CurlReadOp*>(clientp);
    auto fd = me->m_broker_reverse_socket;
    me->m_broker_reverse_socket = -1;
    if (fd == -1) {
        return CURL_SOCKET_BAD;
    } else {
        return fd;
    }
}

int
CurlOperation::SockOptCallback(void *clientp, curl_socket_t curlfd, curlsocktype purpose)
{
    return CURL_SOCKOPT_ALREADY_CONNECTED;
}

int
CurlOperation::XferInfoCallback(void *clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    auto me = reinterpret_cast<CurlOperation*>(clientp);
    if (me->HeaderTimeoutExpired()) {
        return 1;
    }
    return 0;
}

int
CurlOperation::WaitSocketCallback(std::string &err)
{
    m_broker_reverse_socket = m_broker ? m_broker->FinishRequest(err) : -1;
    if (m_broker && m_broker_reverse_socket == -1) {
        m_logger->Error(kLogXrdClPelican, "Error when getting socket from parent: %s", err.c_str());
    } else if (m_broker) {
        m_logger->Debug(kLogXrdClPelican, "Got reverse connection on socket %d", m_broker_reverse_socket);
    }
    return m_broker_reverse_socket;
}

bool
CurlStatOp::Redirect()
{
    auto result = CurlOperation::Redirect();
    if (m_is_pelican) {
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, "PROPFIND");
        m_is_propfind = true;
    } else {
        curl_easy_setopt(m_curl.get(), CURLOPT_NOBODY, 1L);
    }
    return result;
}

void
CurlStatOp::Setup(CURL *curl, CurlWorker &worker)
{
    CurlOperation::Setup(curl, worker);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, CurlStatOp::WriteCallback);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, this);
    if (m_is_origin && m_is_pelican) {
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, "PROPFIND");
        m_is_propfind = true;
    }
}

void
CurlStatOp::ReleaseHandle()
{
    if (m_curl == nullptr) return;
    curl_easy_setopt(m_curl.get(), CURLOPT_NOBODY, 0L);
    if (m_is_propfind) {
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, nullptr);
    }
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, nullptr);
    CurlOperation::ReleaseHandle();
}

size_t
CurlStatOp::WriteCallback(char *buffer, size_t size, size_t nitems, void *this_ptr)
{
    auto me = static_cast<CurlStatOp*>(this_ptr);
    if (me->m_is_propfind) {
        if (size * nitems + me->m_response.size() > 1'000'000) {
            me->m_logger->Error(kLogXrdClPelican, "Response too large for PROPFIND operation");
            return 0;
        }
        me->m_response.append(buffer, size * nitems);
    }
    return size * nitems;
}

std::pair<int64_t, bool>
CurlStatOp::ParseProp(tinyxml2::XMLElement *prop) {
    if (prop == nullptr) {
        return {-1, false};
    }
    for (auto child = prop->FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
        if (!strcmp(child->Name(), "D:getcontentlength") || !strcmp(child->Name(), "lp1:getcontentlength")) {
            auto len = child->GetText();
            if (len) {
                m_length = std::stoll(len);
            }
        } else if (!strcmp(child->Name(), "D:resourcetype") || !strcmp(child->Name(), "lp1:resourcetype")) {
            m_is_dir = child->FirstChildElement("D:collection") != nullptr;
        }
    }
    return {m_length, m_is_dir};
}

std::pair<int64_t, bool>
CurlStatOp::GetStatInfo() {
    if (!m_is_propfind) {
        m_length = m_headers.GetContentLength();
        return {m_length, false};
    }
    if (m_length >= 0) {
        return {m_length, m_is_dir};
    }

    tinyxml2::XMLDocument doc;
    auto err = doc.Parse(m_response.c_str());
    if (err != tinyxml2::XML_SUCCESS) {
        m_logger->Error(kLogXrdClPelican, "Failed to parse XML response: %s", m_response.substr(0, 1024).c_str());
        return {-1, false};
    }

    auto elem = doc.RootElement();
    if (strcmp(elem->Name(), "D:multistatus")) {
        m_logger->Error(kLogXrdClPelican, "Unexpected XML response: %s", m_response.substr(0, 1024).c_str());
        return {-1, false};
    }
    auto found_response = false;
    for (auto response = elem->FirstChildElement(); response != nullptr; response = response->NextSiblingElement()) {
        if (!strcmp(response->Name(), "D:response")) {
            found_response = true;
            elem = response;
            break;
        }
    }
    if (!found_response) {
        m_logger->Error(kLogXrdClPelican, "Failed to find response element in XML response: %s", m_response.substr(0, 1024).c_str());
        return {-1, false};
    }
    for (auto child = elem->FirstChildElement(); child != nullptr; child = child->NextSiblingElement()) {
		if (strcmp(child->Name(), "D:propstat")) {
            continue;
        }
        for (auto prop = child->FirstChildElement(); prop != nullptr; prop = prop->NextSiblingElement()) {
            if (!strcmp(prop->Name(), "D:prop")) {
                return ParseProp(prop);
            }
        }
	}
    m_logger->Error(kLogXrdClPelican, "Failed to find properties in XML response: %s", m_response.substr(0, 1024).c_str());
    return {-1, false};
}

void
CurlStatOp::Success()
{
    SetDone();
    m_logger->Debug(kLogXrdClPelican, "CurlStatOp::Success");
    auto [size, isdir] = GetStatInfo();
    if (size < 0) {
        m_logger->Error(kLogXrdClPelican, "Failed to get stat info for %s", m_url.c_str());
        Fail(XrdCl::errErrorResponse, kXR_FSError, "Server responded without object size");
        return;
    }
    if (m_is_propfind) {
        m_logger->Debug(kLogXrdClPelican, "Successful propfind operation on %s (size %lld, isdir %d)", m_url.c_str(), static_cast<long long>(size), isdir);
    } else {
        m_logger->Debug(kLogXrdClPelican, "Successful stat operation on %s (size %lld)", m_url.c_str(), static_cast<long long>(size));
    }
    if (m_handler == nullptr) {return;}
    auto stat_info = new XrdCl::StatInfo("nobody", size,
        XrdCl::StatInfo::Flags::IsReadable | (isdir ? XrdCl::StatInfo::Flags::IsDir : 0), time(NULL));
    auto obj = new XrdCl::AnyObject();
    obj->Set(stat_info);

    if (m_dcache && !m_is_origin) {
        m_logger->Debug(kLogXrdClPelican, "Will save successful open info to director cache");
        if (!GetMirrorUrl().empty()) {
            m_logger->Debug(kLogXrdClPelican, "Caching response URL %s", GetMirrorUrl().c_str());
            m_dcache->Put(GetMirrorUrl(), GetMirrorDepth());
        } else {
            m_logger->Debug(kLogXrdClPelican, "No link information found in headers");
        }
    } else if (!m_dcache) {
        m_logger->Debug(kLogXrdClPelican, "No director cache available");
    }

    m_handler->HandleResponse(new XrdCl::XRootDStatus(), obj);
    m_handler = nullptr;
}

CurlOpenOp::CurlOpenOp(XrdCl::ResponseHandler *handler, const std::string &url, struct timespec timeout,
    XrdCl::Log *logger, File *file, const DirectorCache *dcache)
:
    CurlStatOp(handler, url, timeout, logger, file->IsPelican(), file->IsCachedUrl(), dcache),
    m_file(file)
{}

void
CurlOpenOp::ReleaseHandle()
{
    curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETDATA, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTDATA, nullptr);
    CurlStatOp::ReleaseHandle();
}

void
CurlOpenOp::Success()
{
    SetDone();
    char *url = nullptr;
    curl_easy_getinfo(m_curl.get(), CURLINFO_EFFECTIVE_URL, &url);
    if (url && m_file) {
        m_file->SetProperty("LastURL", url);
    }
    if (UseX509Auth() && m_file) {
        m_file->SetProperty("UseX509Auth", "true");
    }
    const auto &broker = GetBrokerUrl();
    if (!broker.empty() && m_file) {
        m_file->SetProperty("BrokerURL", broker);
    }
    auto [size, isdir] = GetStatInfo();
    if (isdir) {
        m_logger->Error(kLogXrdClPelican, "Cannot open a directory");
        Fail(XrdCl::errErrorResponse, kXR_isDirectory, "Cannot open a directory");
        return;
    }
    if (size >= 0) {
        m_file->SetProperty("ContentLength", std::to_string(size));
    }
    CurlStatOp::Success();
}

CurlReadOp::CurlReadOp(XrdCl::ResponseHandler *handler, const std::string &url, struct timespec timeout,
    const std::pair<uint64_t, uint64_t> &op, char *buffer, XrdCl::Log *logger) :
        CurlOperation(handler, url, timeout, logger),
        m_op(op),
        m_buffer(buffer),
        m_header_list(nullptr, &curl_slist_free_all)
    {}

void
CurlReadOp::Setup(CURL *curl, CurlWorker &worker)
{
    CurlOperation::Setup(curl, worker);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, CurlReadOp::WriteCallback);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, this);

    // Note: range requests are inclusive of the end byte, meaning "bytes=0-1023" is a 1024-byte request.
    // This is why we subtract '1' off the end.
    if (m_op.second == 0) {
        Success();
        return;
    }
    auto range_req = "Range: bytes=" + std::to_string(m_op.first) + "-" + std::to_string(m_op.first + m_op.second - 1);
    m_header_list.reset(curl_slist_append(m_header_list.release(), range_req.c_str()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, m_header_list.get());
}

void
CurlReadOp::Fail(uint16_t errCode, uint32_t errNum, const std::string &msg)
{
    std::string custom_msg = msg;
    SetDone();
    if (m_handler == nullptr) {return;}
    if (!custom_msg.empty()) {
        m_logger->Debug(kLogXrdClPelican, "curl operation at offset %llu failed with message: %s", static_cast<long long unsigned>(m_op.first), msg.c_str());
        custom_msg += " (read operation at offset " + std::to_string(static_cast<long long unsigned>(m_op.first)) + ")";
    } else {
        m_logger->Debug(kLogXrdClPelican, "curl operation at offset %llu failed with status code %d", static_cast<long long unsigned>(m_op.first), errNum);
    }
    auto status = new XrdCl::XRootDStatus(XrdCl::stError, errCode, errNum, custom_msg);
    m_handler->HandleResponse(status, nullptr);
    m_handler = nullptr;
}

void
CurlReadOp::Success()
{
    SetDone();
    if (m_handler == nullptr) {return;}
    auto status = new XrdCl::XRootDStatus();
    auto chunk_info = new XrdCl::ChunkInfo(m_op.first, m_written, m_buffer);
    auto obj = new XrdCl::AnyObject();
    obj->Set(chunk_info);
    m_handler->HandleResponse(status, obj);
    m_handler = nullptr;
}

void
CurlReadOp::ReleaseHandle()
{
    if (m_curl == nullptr) return;
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_OPENSOCKETDATA, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_SOCKOPTDATA, nullptr);
    m_header_list.reset();
    CurlOperation::ReleaseHandle();
}

size_t
CurlReadOp::WriteCallback(char *buffer, size_t size, size_t nitems, void *this_ptr)
{
    return static_cast<CurlReadOp*>(this_ptr)->Write(buffer, size * nitems);
}

size_t
CurlReadOp::Write(char *buffer, size_t length)
{
    //m_logger->Debug(kLogXrdClPelican, "Received a write of size %d with offset %d; total received is %d; remaining is %d", length, m_op.first, length + m_written, m_op.second - length - m_written);
    if (m_headers.IsMultipartByterange()) {
        Fail(XrdCl::errErrorResponse, kXR_ServerError, "Server responded with a multipart byterange which is not supported");
        return 0;
    }
    if (m_written == 0 && (m_headers.GetOffset() != m_op.first)) {
        Fail(XrdCl::errErrorResponse, kXR_ServerError, "Server did not return content with correct offset");
        return 0;
    }
    if (m_written + length > m_op.second) { // We don't have enough space in the buffer to write the resp.
        Fail(XrdCl::errErrorResponse, kXR_ServerError, "Server sent back more data than requested");
        return 0;
    }
    memcpy(m_buffer + m_written, buffer, length);
    m_written += length;
    return length;
}

void                
CurlPgReadOp::Success()
{               
    SetDone();
    if (m_handler == nullptr) {return;}
    auto status = new XrdCl::XRootDStatus();

    std::vector<uint32_t> cksums;
    size_t nbpages = m_written / XrdSys::PageSize;
    if (m_written % XrdSys::PageSize) ++nbpages;
    cksums.reserve(nbpages);

    auto buffer = m_buffer;
    size_t size = m_written;
    for (size_t pg=0; pg<nbpages; ++pg)
    {
        auto pgsize = static_cast<size_t>(XrdSys::PageSize);
        if (pgsize > size) pgsize = size;
        cksums.push_back(XrdOucCRC::Calc32C(buffer, pgsize));
        buffer += pgsize;
        size -= pgsize;
    }

    auto page_info = new XrdCl::PageInfo(m_op.first, m_written, m_buffer, std::move(cksums));
    auto obj = new XrdCl::AnyObject();
    obj->Set(page_info);
    m_handler->HandleResponse(status, obj);
    m_handler = nullptr;
}               

CurlListdirOp::CurlListdirOp(XrdCl::ResponseHandler *handler, const std::string &url, const std::string &host_addr, bool is_origin, struct timespec timeout,
    XrdCl::Log *logger) :
    CurlOperation(handler, url, timeout, logger),
    m_is_origin(is_origin),
    m_host_addr(host_addr),
    m_header_list(nullptr, &curl_slist_free_all)
{}

void
CurlListdirOp::Setup(CURL *curl, CurlWorker &worker)
{
    CurlOperation::Setup(curl, worker);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, CurlListdirOp::WriteCallback);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, this);
    curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, "PROPFIND");
    m_header_list.reset(curl_slist_append(m_header_list.release(), "Depth: 1"));
    curl_easy_setopt(m_curl.get(), CURLOPT_HTTPHEADER, m_header_list.get());
}

void
CurlListdirOp::ReleaseHandle()
{
    if (m_curl == nullptr) return;
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(m_curl.get(), CURLOPT_HTTPHEADER, nullptr);
    m_header_list.reset();
    CurlOperation::ReleaseHandle();
}

size_t
CurlListdirOp::WriteCallback(char *buffer, size_t size, size_t nitems, void *this_ptr)
{
    auto me = static_cast<CurlListdirOp*>(this_ptr);
    if (size * nitems + me->m_response.size() > 10'000'000) {
        me->m_logger->Error(kLogXrdClPelican, "Response too large for PROPFIND operation");
        return 0;
    }
    me->m_response.append(buffer, size * nitems);
    return size * nitems;
}
