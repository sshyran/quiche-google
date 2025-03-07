#include "quiche/http2/adapter/oghttp2_adapter.h"

#include <string>

#include "absl/strings/str_join.h"
#include "quiche/http2/adapter/http2_protocol.h"
#include "quiche/http2/adapter/http2_visitor_interface.h"
#include "quiche/http2/adapter/mock_http2_visitor.h"
#include "quiche/http2/adapter/oghttp2_util.h"
#include "quiche/http2/adapter/test_frame_sequence.h"
#include "quiche/http2/adapter/test_utils.h"
#include "quiche/common/platform/api/quiche_expect_bug.h"
#include "quiche/common/platform/api/quiche_test.h"

namespace http2 {
namespace adapter {
namespace test {
namespace {

using ConnectionError = Http2VisitorInterface::ConnectionError;

using spdy::SpdyFrameType;
using testing::_;

enum FrameType {
  DATA,
  HEADERS,
  PRIORITY,
  RST_STREAM,
  SETTINGS,
  PUSH_PROMISE,
  PING,
  GOAWAY,
  WINDOW_UPDATE,
  CONTINUATION,
};

// TODO(birenroy): replace numeric flag values with named constants
enum FrameFlag {
  END_STREAM = 0x01,
  ACK = END_STREAM,
  END_HEADERS = 0x04,
};

TEST(OgHttp2AdapterTest, IsServerSession) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_TRUE(adapter->IsServerSession());
}

TEST(OgHttp2AdapterTest, ProcessBytes) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence seq;
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, 4, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 8, 6, 0));
  EXPECT_CALL(visitor, OnPing(17, false));
  adapter->ProcessBytes(
      TestFrameSequence().ClientPreface().Ping(17).Serialize());
}

TEST(OgHttp2AdapterTest, HeaderValuesWithObsTextAllowedByDefault) {
  DataSavingVisitor visitor;
  OgHttp2Session::Options options;
  options.perspective = Perspective::kServer;
  ASSERT_TRUE(options.allow_obs_text);
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/"},
                                           {"name", "val\xa1ue"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "name", "val\xa1ue"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));
}

TEST(OgHttp2AdapterTest, HeaderValuesWithObsTextDisallowed) {
  DataSavingVisitor visitor;
  OgHttp2Session::Options options;
  options.allow_obs_text = false;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/"},
                                           {"name", "val\xa1ue"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/"));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));
}

TEST(OgHttp2AdapterTest, InitialSettingsNoExtendedConnect) {
  DataSavingVisitor client_visitor;
  OgHttp2Adapter::Options client_options;
  client_options.perspective = Perspective::kClient;
  client_options.max_header_list_bytes = 42;
  client_options.allow_extended_connect = false;
  auto client_adapter = OgHttp2Adapter::Create(client_visitor, client_options);

  DataSavingVisitor server_visitor;
  OgHttp2Adapter::Options server_options;
  server_options.perspective = Perspective::kServer;
  server_options.allow_extended_connect = false;
  auto server_adapter = OgHttp2Adapter::Create(server_visitor, server_options);

  testing::InSequence s;

  // Client sends the connection preface, including the initial SETTINGS.
  EXPECT_CALL(client_visitor, OnBeforeFrameSent(SETTINGS, 0, 12, 0x0));
  EXPECT_CALL(client_visitor, OnFrameSent(SETTINGS, 0, 12, 0x0, 0));
  {
    int result = client_adapter->Send();
    EXPECT_EQ(0, result);
    absl::string_view data = client_visitor.data();
    EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
    data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
    EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));
  }

  // Server sends the connection preface, including the initial SETTINGS.
  EXPECT_CALL(server_visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x0));
  EXPECT_CALL(server_visitor, OnFrameSent(SETTINGS, 0, 0, 0x0, 0));
  {
    int result = server_adapter->Send();
    EXPECT_EQ(0, result);
    absl::string_view data = server_visitor.data();
    EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));
  }

  // Client processes the server's initial bytes, including initial SETTINGS.
  EXPECT_CALL(client_visitor, OnFrameHeader(0, 0, SETTINGS, 0x0));
  EXPECT_CALL(client_visitor, OnSettingsStart());
  EXPECT_CALL(client_visitor, OnSettingsEnd());
  {
    const int64_t result = client_adapter->ProcessBytes(server_visitor.data());
    EXPECT_EQ(server_visitor.data().size(), static_cast<size_t>(result));
  }

  // Server processes the client's initial bytes, including initial SETTINGS.
  EXPECT_CALL(server_visitor, OnFrameHeader(0, 12, SETTINGS, 0x0));
  EXPECT_CALL(server_visitor, OnSettingsStart());
  EXPECT_CALL(server_visitor,
              OnSetting(Http2Setting{Http2KnownSettingsId::ENABLE_PUSH, 0u}));
  EXPECT_CALL(
      server_visitor,
      OnSetting(Http2Setting{Http2KnownSettingsId::MAX_HEADER_LIST_SIZE, 42u}));
  EXPECT_CALL(server_visitor, OnSettingsEnd());
  {
    const int64_t result = server_adapter->ProcessBytes(client_visitor.data());
    EXPECT_EQ(client_visitor.data().size(), static_cast<size_t>(result));
  }
}

TEST(OgHttp2AdapterTest, InitialSettings) {
  DataSavingVisitor client_visitor;
  OgHttp2Adapter::Options client_options;
  client_options.perspective = Perspective::kClient;
  client_options.max_header_list_bytes = 42;
  ASSERT_TRUE(client_options.allow_extended_connect);
  auto client_adapter = OgHttp2Adapter::Create(client_visitor, client_options);

  DataSavingVisitor server_visitor;
  OgHttp2Adapter::Options server_options;
  server_options.perspective = Perspective::kServer;
  ASSERT_TRUE(server_options.allow_extended_connect);
  auto server_adapter = OgHttp2Adapter::Create(server_visitor, server_options);

  testing::InSequence s;

  // Client sends the connection preface, including the initial SETTINGS.
  EXPECT_CALL(client_visitor, OnBeforeFrameSent(SETTINGS, 0, 12, 0x0));
  EXPECT_CALL(client_visitor, OnFrameSent(SETTINGS, 0, 12, 0x0, 0));
  {
    int result = client_adapter->Send();
    EXPECT_EQ(0, result);
    absl::string_view data = client_visitor.data();
    EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
    data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
    EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));
  }

  // Server sends the connection preface, including the initial SETTINGS.
  EXPECT_CALL(server_visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(server_visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  {
    int result = server_adapter->Send();
    EXPECT_EQ(0, result);
    absl::string_view data = server_visitor.data();
    EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));
  }

  // Client processes the server's initial bytes, including initial SETTINGS.
  EXPECT_CALL(client_visitor, OnFrameHeader(0, 6, SETTINGS, 0x0));
  EXPECT_CALL(client_visitor, OnSettingsStart());
  EXPECT_CALL(client_visitor,
              OnSetting(Http2Setting{
                  Http2KnownSettingsId::ENABLE_CONNECT_PROTOCOL, 1u}));
  EXPECT_CALL(client_visitor, OnSettingsEnd());
  {
    const int64_t result = client_adapter->ProcessBytes(server_visitor.data());
    EXPECT_EQ(server_visitor.data().size(), static_cast<size_t>(result));
  }

  // Server processes the client's initial bytes, including initial SETTINGS.
  EXPECT_CALL(server_visitor, OnFrameHeader(0, 12, SETTINGS, 0x0));
  EXPECT_CALL(server_visitor, OnSettingsStart());
  EXPECT_CALL(server_visitor,
              OnSetting(Http2Setting{Http2KnownSettingsId::ENABLE_PUSH, 0u}));
  EXPECT_CALL(
      server_visitor,
      OnSetting(Http2Setting{Http2KnownSettingsId::MAX_HEADER_LIST_SIZE, 42u}));
  EXPECT_CALL(server_visitor, OnSettingsEnd());
  {
    const int64_t result = server_adapter->ProcessBytes(client_visitor.data());
    EXPECT_EQ(client_visitor.data().size(), static_cast<size_t>(result));
  }
}

TEST(OgHttp2AdapterTest, AutomaticSettingsAndPingAcks) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface().Ping(42).Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // PING
  EXPECT_CALL(visitor, OnFrameHeader(0, _, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  // Server preface (SETTINGS)
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  // SETTINGS ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  // PING ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(PING, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(PING, 0, _, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::PING}));
}

TEST(OgHttp2AdapterTest, AutomaticPingAcksDisabled) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.auto_ping_ack = false;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface().Ping(42).Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // PING
  EXPECT_CALL(visitor, OnFrameHeader(0, _, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  // Server preface (SETTINGS)
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  // SETTINGS ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  // No PING ack expected because automatic PING acks are disabled.

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, InvalidMaxFrameSizeSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface({{MAX_FRAME_SIZE, 3u}}).Serialize();
  testing::InSequence s;

  // Client preface
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(0, Http2VisitorInterface::InvalidFrameError::kProtocol));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidSetting));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, InvalidPushSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface({{ENABLE_PUSH, 3u}}).Serialize();
  testing::InSequence s;

  // Client preface
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(0, Http2VisitorInterface::InvalidFrameError::kProtocol));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidSetting));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, InvalidConnectProtocolSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface({{ENABLE_CONNECT_PROTOCOL, 3u}})
                                 .Serialize();
  testing::InSequence s;

  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(0, Http2VisitorInterface::InvalidFrameError::kProtocol));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidSetting));

  int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));

  auto adapter2 = OgHttp2Adapter::Create(visitor, options);
  const std::string frames2 = TestFrameSequence()
                                  .ClientPreface({{ENABLE_CONNECT_PROTOCOL, 1}})
                                  .Settings({{ENABLE_CONNECT_PROTOCOL, 0}})
                                  .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{ENABLE_CONNECT_PROTOCOL, 1u}));
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(0, Http2VisitorInterface::InvalidFrameError::kProtocol));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidSetting));

  read_result = adapter2->ProcessBytes(frames2);
  EXPECT_EQ(static_cast<size_t>(read_result), frames2.size());

  EXPECT_TRUE(adapter2->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  adapter2->Send();
}

TEST(OgHttp2AdapterTest, ClientHandles100Headers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "100"}},
                   /*fin=*/false)
          .Ping(101)
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "100"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0));
  EXPECT_CALL(visitor, OnPing(101, false));

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(PING, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(PING, 0, _, 0x1, 0));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::PING}));
}

TEST(OgHttp2AdapterTest, QueuingWindowUpdateAffectsWindow) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_EQ(adapter->GetReceiveWindowSize(), kInitialFlowControlWindowSize);
  adapter->SubmitWindowUpdate(0, 10000);
  EXPECT_EQ(adapter->GetReceiveWindowSize(),
            kInitialFlowControlWindowSize + 10000);

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});
  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(WINDOW_UPDATE, 0, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(WINDOW_UPDATE, 0, 4, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);

  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id),
            kInitialFlowControlWindowSize);
  adapter->SubmitWindowUpdate(1, 20000);
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id),
            kInitialFlowControlWindowSize + 20000);
}

TEST(OgHttp2AdapterTest, AckOfSettingInitialWindowSizeAffectsWindow) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});
  const int32_t stream_id1 = adapter->SubmitRequest(headers, nullptr, nullptr);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);

  const std::string initial_frames =
      TestFrameSequence()
          .ServerPreface()
          .SettingsAck()  // Ack of the client's initial settings.
          .Serialize();
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck);

  int64_t parse_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(parse_result));

  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id1),
            kInitialFlowControlWindowSize);
  adapter->SubmitSettings({{INITIAL_WINDOW_SIZE, 80000u}});
  // No update for the first stream, yet.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id1),
            kInitialFlowControlWindowSize);

  // Ack of server's initial settings.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  // Outbound SETTINGS containing INITIAL_WINDOW_SIZE.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);

  // Still no update, as a SETTINGS ack has not yet been received.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id1),
            kInitialFlowControlWindowSize);

  const std::string settings_ack =
      TestFrameSequence().SettingsAck().Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck);

  parse_result = adapter->ProcessBytes(settings_ack);
  EXPECT_EQ(settings_ack.size(), static_cast<size_t>(parse_result));

  // Stream window has been updated.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id1), 80000);

  const std::vector<Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});
  const int32_t stream_id2 = adapter->SubmitRequest(headers, nullptr, nullptr);

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id2, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id2, _, 0x5, 0));
  result = adapter->Send();
  EXPECT_EQ(0, result);

  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(stream_id2), 80000);
}

TEST(OgHttp2AdapterTest, ClientRejects100HeadersWithFin) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "100"}}, /*fin=*/false)
          .Headers(1, {{":status", "100"}}, /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "100"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "100"));
  EXPECT_CALL(visitor,
              OnInvalidFrame(
                  1, Http2VisitorInterface::InvalidFrameError::kHttpMessaging));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(RST_STREAM, 1, _, 0x0, 1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientRejects100HeadersWithContent) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "100"}},
                   /*fin=*/false)
          .Data(1, "We needed the final headers before data, whoops")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "100"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, _));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientRejects100HeadersWithContentLength) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "100"}, {"content-length", "42"}},
                   /*fin=*/false)
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "100"));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientHandles204WithContent) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  const std::vector<Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});

  const int32_t stream_id2 = adapter->SubmitRequest(headers2, nullptr, nullptr);
  ASSERT_GT(stream_id2, stream_id1);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id2, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id2, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "204"}, {"content-length", "2"}},
                   /*fin=*/false)
          .Data(1, "hi")
          .Headers(3, {{":status", "204"}}, /*fin=*/false)
          .Data(3, "hi")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "204"));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":status", "204"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(3, 2));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::RST_STREAM,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientHandles304WithContent) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "304"}, {"content-length", "2"}},
                   /*fin=*/false)
          .Data(1, "hi")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "304"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "content-length", "2"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 2));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientHandles304WithContentLength) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "304"}, {"content-length", "2"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "304"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "content-length", "2"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ClientHandlesTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Headers(1, {{"final-status", "A-OK"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "final-status", "A-OK"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ClientSendsTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const std::string kBody = "This is an example request body.";
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->AppendPayload(kBody);
  body1->EndData();

  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, std::move(body1), nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id1, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS,
                            SpdyFrameType::DATA}));
  visitor.Clear();

  const std::vector<Header> trailers1 =
      ToHeaders({{"extra-info", "Trailers are weird but good?"}});
  adapter->SubmitTrailer(stream_id1, trailers1);

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  data = visitor.data();
  EXPECT_THAT(data, EqualsFrames({SpdyFrameType::HEADERS}));
}

TEST(OgHttp2AdapterTest, ClientHandlesMetadata) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Metadata(0, "Example connection metadata")
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Metadata(1, "Example stream metadata")
          .Data(1, "This is the response body.", true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(0));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ClientHandlesMetadataWithEmptyPayload) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Metadata(1, "")
          .Data(1, "This is the response body.", true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(3);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, _));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));
}

TEST(OgHttp2AdapterTest, ClientHandlesMetadataWithPayloadError) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Metadata(0, "Example connection metadata")
          .Headers(stream_id,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Metadata(stream_id, "Example stream metadata")
          .Data(stream_id, "This is the response body.", true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(0));
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnHeaderForStream(stream_id, _, _)).Times(3);
  EXPECT_CALL(visitor, OnEndHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(stream_id, _));
  EXPECT_CALL(visitor, OnMetadataForStream(stream_id, _))
      .WillOnce(testing::Return(false));
  // Remaining frames are not processed due to the error.
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  // Negative integer returned to indicate an error.
  EXPECT_LT(stream_result, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  EXPECT_FALSE(adapter->want_read());
  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientHandlesMetadataWithCompletionError) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Metadata(0, "Example connection metadata")
          .Headers(stream_id,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Metadata(stream_id, "Example stream metadata")
          .Data(stream_id, "This is the response body.", true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(0));
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnHeaderForStream(stream_id, _, _)).Times(3);
  EXPECT_CALL(visitor, OnEndHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(stream_id, _));
  EXPECT_CALL(visitor, OnMetadataForStream(stream_id, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(stream_id))
      .WillOnce(testing::Return(false));
  // Remaining frames are not processed due to the error.
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  // Negative integer returned to indicate an error.
  EXPECT_LT(stream_result, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  EXPECT_FALSE(adapter->want_read());
  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientRstStreamWhileHandlingHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"))
      .WillOnce(testing::DoAll(
          testing::InvokeWithoutArgs([&adapter]() {
            adapter->SubmitRst(1, Http2ErrorCode::REFUSED_STREAM);
          }),
          testing::Return(Http2VisitorInterface::HEADER_RST_STREAM)));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, stream_id1, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, stream_id1, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::REFUSED_STREAM)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientConnectionErrorWhileHandlingHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"))
      .WillOnce(
          testing::Return(Http2VisitorInterface::HEADER_CONNECTION_ERROR));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kHeaderError));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_LT(stream_result, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientConnectionErrorWhileHandlingHeadersOnly) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"))
      .WillOnce(
          testing::Return(Http2VisitorInterface::HEADER_CONNECTION_ERROR));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kHeaderError));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_LT(stream_result, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientRejectsHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1))
      .WillOnce(testing::Return(false));
  // Rejecting headers leads to a connection error.
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kHeaderError));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_LT(stream_result, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientHandlesSmallerHpackHeaderTableSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 = ToHeaders({
      {":method", "GET"},
      {":scheme", "http"},
      {":authority", "example.com"},
      {":path", "/this/is/request/one"},
      {"x-i-do-not-like", "green eggs and ham"},
      {"x-i-will-not-eat-them", "here or there, in a box, with a fox"},
      {"x-like-them-in-a-house", "no"},
      {"x-like-them-with-a-mouse", "no"},
  });

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  EXPECT_GT(adapter->GetHpackEncoderDynamicTableSize(), 100);

  const std::string stream_frames =
      TestFrameSequence().Settings({{HEADER_TABLE_SIZE, 100u}}).Serialize();
  // Server preface (SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{HEADER_TABLE_SIZE, 100u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_EQ(adapter->GetHpackEncoderDynamicTableCapacity(), 100);
  EXPECT_LE(adapter->GetHpackEncoderDynamicTableSize(), 100);
}

TEST(OgHttp2AdapterTest, ClientHandlesLargerHpackHeaderTableSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  EXPECT_EQ(adapter->GetHpackEncoderDynamicTableCapacity(), 4096);

  const std::string stream_frames =
      TestFrameSequence().Settings({{HEADER_TABLE_SIZE, 40960u}}).Serialize();
  // Server preface (SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{HEADER_TABLE_SIZE, 40960u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  // The increased capacity will not be applied until a SETTINGS ack is
  // serialized.
  EXPECT_EQ(adapter->GetHpackEncoderDynamicTableCapacity(), 4096);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  EXPECT_EQ(adapter->GetHpackEncoderDynamicTableCapacity(), 40960);
}

TEST(OgHttp2AdapterTest, ClientSendsHpackHeaderTableSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 = ToHeaders({
      {":method", "GET"},
      {":scheme", "http"},
      {":authority", "example.com"},
      {":path", "/this/is/request/one"},
  });

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .SettingsAck()
          .Headers(
              1,
              {{":status", "200"},
               {"server", "my-fake-server"},
               {"date", "Tue, 6 Apr 2021 12:54:01 GMT"},
               {"x-i-do-not-like", "green eggs and ham"},
               {"x-i-will-not-eat-them", "here or there, in a box, with a fox"},
               {"x-like-them-in-a-house", "no"},
               {"x-like-them-with-a-mouse", "no"}},
              /*fin=*/true)
          .Serialize();
  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Server acks client's initial SETTINGS.
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 1));
  EXPECT_CALL(visitor, OnSettingsAck());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(7);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_GT(adapter->GetHpackDecoderSizeLimit(), 100);

  // Submit settings, check decoder table size.
  adapter->SubmitSettings({{HEADER_TABLE_SIZE, 100u}});
  EXPECT_GT(adapter->GetHpackDecoderSizeLimit(), 100);

  // Server preface SETTINGS ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  // SETTINGS with the new header table size value
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));

  // Because the client has not yet seen an ack from the server for the SETTINGS
  // with header table size, it has not applied the new value.
  EXPECT_GT(adapter->GetHpackDecoderSizeLimit(), 100);

  result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::vector<Header> headers2 = ToHeaders({
      {":method", "GET"},
      {":scheme", "http"},
      {":authority", "example.com"},
      {":path", "/this/is/request/two"},
  });

  const int32_t stream_id2 = adapter->SubmitRequest(headers2, nullptr, nullptr);
  ASSERT_GT(stream_id2, stream_id1);

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id2, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id2, _, 0x5, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string response_frames =
      TestFrameSequence()
          .Headers(stream_id2,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(stream_id2, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(stream_id2));
  EXPECT_CALL(visitor, OnHeaderForStream(stream_id2, _, _)).Times(3);
  EXPECT_CALL(visitor, OnEndHeadersForStream(stream_id2));
  EXPECT_CALL(visitor, OnEndStream(stream_id2));
  EXPECT_CALL(visitor,
              OnCloseStream(stream_id2, Http2ErrorCode::HTTP2_NO_ERROR));

  const int64_t response_result = adapter->ProcessBytes(response_frames);
  EXPECT_EQ(response_frames.size(), static_cast<size_t>(response_result));

  // Still no ack for the outbound settings.
  EXPECT_GT(adapter->GetHpackDecoderSizeLimit(), 100);

  const std::string settings_ack =
      TestFrameSequence().SettingsAck().Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 1));
  EXPECT_CALL(visitor, OnSettingsAck());

  const int64_t ack_result = adapter->ProcessBytes(settings_ack);
  EXPECT_EQ(settings_ack.size(), static_cast<size_t>(ack_result));
  // Ack has finally arrived.
  EXPECT_EQ(adapter->GetHpackDecoderSizeLimit(), 100);
}

// TODO(birenroy): Validate headers and re-enable this test. The library should
// invoke OnErrorDebug() with an error message for the invalid header. The
// library should also invoke OnInvalidFrame() for the invalid HEADERS frame.
TEST(OgHttp2AdapterTest, DISABLED_ClientHandlesInvalidTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.")
          .Headers(1, {{":bad-status", "9000"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 26, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 26));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the response body."));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));

  // Bad status trailer will cause a PROTOCOL_ERROR. The header is never
  // delivered in an OnHeaderForStream callback.

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, stream_id1, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(RST_STREAM, stream_id1, 4, 0x0, 1));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::PROTOCOL_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientStartsShutdown) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_FALSE(adapter->want_write());

  // No-op (except for logging) for a client implementation.
  adapter->SubmitShutdownNotice();
  EXPECT_FALSE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);

  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized, EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ClientReceivesGoAway) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  const std::vector<Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});

  const int32_t stream_id2 = adapter->SubmitRequest(headers2, nullptr, nullptr);
  ASSERT_GT(stream_id2, stream_id1);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id2, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id2, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS,
                            SpdyFrameType::HEADERS}));
  visitor.Clear();

  // Submit a pending WINDOW_UPDATE for a stream that will be closed due to
  // GOAWAY. The WINDOW_UPDATE should not be sent.
  adapter->SubmitWindowUpdate(3, 42);

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .RstStream(1, Http2ErrorCode::ENHANCE_YOUR_CALM)
          .GoAway(1, Http2ErrorCode::INTERNAL_ERROR, "indigestion")
          .WindowUpdate(0, 42)
          .WindowUpdate(1, 42)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(1, 4, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnRstStream(1, Http2ErrorCode::ENHANCE_YOUR_CALM));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::ENHANCE_YOUR_CALM));
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  // Currently, oghttp2 does not pass the opaque data to the visitor.
  EXPECT_CALL(visitor, OnGoAway(1, Http2ErrorCode::INTERNAL_ERROR, ""));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::REFUSED_STREAM));
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 42));
  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ClientReceivesMultipleGoAways) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string initial_frames =
      TestFrameSequence()
          .ServerPreface()
          .GoAway(kMaxStreamId, Http2ErrorCode::INTERNAL_ERROR, "indigestion")
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  // Currently, oghttp2 does not pass the opaque data to the visitor.
  EXPECT_CALL(visitor,
              OnGoAway(kMaxStreamId, Http2ErrorCode::INTERNAL_ERROR, ""));

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  // Submit a WINDOW_UPDATE for the open stream. Because the stream is below the
  // GOAWAY's last_stream_id, it should be sent.
  adapter->SubmitWindowUpdate(1, 42);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(WINDOW_UPDATE, 1, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(WINDOW_UPDATE, 1, 4, 0x0, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::WINDOW_UPDATE}));
  visitor.Clear();

  const std::string final_frames =
      TestFrameSequence()
          .GoAway(0, Http2ErrorCode::INTERNAL_ERROR, "indigestion")
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  // Currently, oghttp2 does not pass the opaque data to the visitor.
  EXPECT_CALL(visitor, OnGoAway(0, Http2ErrorCode::INTERNAL_ERROR, ""));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::REFUSED_STREAM));

  const int64_t final_result = adapter->ProcessBytes(final_frames);
  EXPECT_EQ(final_frames.size(), static_cast<size_t>(final_result));

  EXPECT_FALSE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), testing::IsEmpty());
}

TEST(OgHttp2AdapterTest, ClientReceivesMultipleGoAwaysWithIncreasingStreamId) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string frames =
      TestFrameSequence()
          .ServerPreface()
          .GoAway(0, Http2ErrorCode::HTTP2_NO_ERROR, "")
          .GoAway(0, Http2ErrorCode::ENHANCE_YOUR_CALM, "")
          .GoAway(1, Http2ErrorCode::INTERNAL_ERROR, "")
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  EXPECT_CALL(visitor, OnGoAway(0, Http2ErrorCode::HTTP2_NO_ERROR, ""));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::REFUSED_STREAM));
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  EXPECT_CALL(visitor, OnGoAway(0, Http2ErrorCode::ENHANCE_YOUR_CALM, ""));
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(0, Http2VisitorInterface::InvalidFrameError::kProtocol));
  // The oghttp2 stack also signals the error via OnConnectionError().
  EXPECT_CALL(visitor,
              OnConnectionError(ConnectionError::kInvalidGoAwayLastStreamId));

  const int64_t frames_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(frames_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientReceivesGoAwayWithPendingStreams) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const std::string initial_frames =
      TestFrameSequence()
          .ServerPreface({{MAX_CONCURRENT_STREAMS, 1}})
          .Serialize();

  // Server preface (SETTINGS with MAX_CONCURRENT_STREAMS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting);
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  const std::vector<Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});

  const int32_t stream_id2 = adapter->SubmitRequest(headers2, nullptr, nullptr);
  ASSERT_GT(stream_id2, stream_id1);

  // The second request should be pending because of
  // SETTINGS_MAX_CONCURRENT_STREAMS.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  // Let the client receive a GOAWAY and raise MAX_CONCURRENT_STREAMS. Even
  // though the GOAWAY last_stream_id is higher than the pending request's
  // stream ID, pending request should not be sent.
  const std::string stream_frames =
      TestFrameSequence()
          .GoAway(kMaxStreamId, Http2ErrorCode::INTERNAL_ERROR, "indigestion")
          .Settings({{MAX_CONCURRENT_STREAMS, 42u}})
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  EXPECT_CALL(visitor,
              OnGoAway(kMaxStreamId, Http2ErrorCode::INTERNAL_ERROR, ""));
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{MAX_CONCURRENT_STREAMS, 42u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  // We close the pending stream on the next write attempt.
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::REFUSED_STREAM));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // Requests submitted after receiving the GOAWAY should not be sent.
  const std::vector<Header> headers3 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/three"}});

  const int32_t stream_id3 = adapter->SubmitRequest(headers3, nullptr, nullptr);
  ASSERT_GT(stream_id3, stream_id2);

  // We close the pending stream on the next write attempt.
  EXPECT_CALL(visitor, OnCloseStream(5, Http2ErrorCode::REFUSED_STREAM));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), testing::IsEmpty());
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, ClientFailsOnGoAway) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const char* kSentinel1 = "arbitrary pointer 1";
  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, nullptr, const_cast<char*>(kSentinel1));
  ASSERT_GT(stream_id1, 0);
  QUICHE_LOG(INFO) << "Created stream: " << stream_id1;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .GoAway(1, Http2ErrorCode::INTERNAL_ERROR, "indigestion")
          .Data(1, "This is the response body.")
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "server", "my-fake-server"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(1, "date", "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0));
  // TODO(birenroy): Pass the GOAWAY opaque data through the oghttp2 stack.
  EXPECT_CALL(visitor, OnGoAway(1, Http2ErrorCode::INTERNAL_ERROR, ""))
      .WillOnce(testing::Return(false));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_LT(stream_result, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientRejects101Response) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"},
                 {"upgrade", "new-protocol"}});

  const int32_t stream_id1 = adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string stream_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1,
                   {{":status", "101"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(static_cast<int64_t>(stream_frames.size()), stream_result);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(RST_STREAM, 1, 4, 0x0,
                  static_cast<uint32_t>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS,
                                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ClientObeysMaxConcurrentStreams) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_FALSE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  // Even though the user has not queued any frames for the session, it should
  // still send the connection preface.
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  // Initial SETTINGS.
  EXPECT_THAT(serialized, EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const std::string initial_frames =
      TestFrameSequence()
          .ServerPreface({{MAX_CONCURRENT_STREAMS, 1}})
          .Serialize();
  testing::InSequence s;

  // Server preface (SETTINGS with MAX_CONCURRENT_STREAMS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting);
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  // Session will want to write a SETTINGS ack.
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const std::string kBody = "This is an example request body.";
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, true);
  body1->AppendPayload(kBody);
  body1->EndData();
  const int stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "POST"},
                                        {":scheme", "http"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             std::move(body1), nullptr);
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id, _, 0x1, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::HEADERS, SpdyFrameType::DATA}));
  EXPECT_THAT(visitor.data(), testing::HasSubstr(kBody));
  visitor.Clear();
  EXPECT_FALSE(adapter->want_write());

  const int next_stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "POST"},
                                        {":scheme", "http"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/two"}}),
                             nullptr, nullptr);

  // A new pending stream is created, but because of MAX_CONCURRENT_STREAMS, the
  // session should not want to write it at the moment.
  EXPECT_GT(next_stream_id, stream_id);
  EXPECT_FALSE(adapter->want_write());

  const std::string stream_frames =
      TestFrameSequence()
          .Headers(stream_id,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(stream_id, "This is the response body.", /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnHeaderForStream(stream_id, ":status", "200"));
  EXPECT_CALL(visitor,
              OnHeaderForStream(stream_id, "server", "my-fake-server"));
  EXPECT_CALL(visitor, OnHeaderForStream(stream_id, "date",
                                         "Tue, 6 Apr 2021 12:54:01 GMT"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, 26, DATA, 0x1));
  EXPECT_CALL(visitor, OnBeginDataForStream(stream_id, 26));
  EXPECT_CALL(visitor,
              OnDataForStream(stream_id, "This is the response body."));
  EXPECT_CALL(visitor, OnEndStream(stream_id));
  EXPECT_CALL(visitor,
              OnCloseStream(stream_id, Http2ErrorCode::HTTP2_NO_ERROR));

  // The first stream should close, which should make the session want to write
  // the next stream.
  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, next_stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, next_stream_id, _, 0x5, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);

  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
  visitor.Clear();
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, ClientReceivesInitialWindowSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string initial_frames =
      TestFrameSequence()
          .Settings({{INITIAL_WINDOW_SIZE, 80000u}})
          .WindowUpdate(0, 65536)
          .Serialize();
  // Server preface (SETTINGS with INITIAL_STREAM_WINDOW)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{INITIAL_WINDOW_SIZE, 80000u}));
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 65536));

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  // Session will want to write a SETTINGS ack.
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  int64_t result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const std::string kLongBody = std::string(81000, 'c');
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, true);
  body1->AppendPayload(kLongBody);
  body1->EndData();
  const int stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "POST"},
                                        {":scheme", "http"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             std::move(body1), nullptr);
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x4, 0));
  // The client can send more than 4 frames (65536 bytes) of data.
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id, 16384, 0x0, 0)).Times(4);
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id, 14464, 0x0, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::HEADERS, SpdyFrameType::DATA,
                            SpdyFrameType::DATA, SpdyFrameType::DATA,
                            SpdyFrameType::DATA, SpdyFrameType::DATA}));
}

TEST(OgHttp2AdapterTest, ClientReceivesInitialWindowSettingAfterStreamStart) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string initial_frames =
      TestFrameSequence().ServerPreface().WindowUpdate(0, 65536).Serialize();
  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 65536));

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  // Session will want to write a SETTINGS ack.
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  int64_t result = adapter->Send();
  EXPECT_EQ(0, result);
  visitor.Clear();

  const std::string kLongBody = std::string(81000, 'c');
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, true);
  body1->AppendPayload(kLongBody);
  body1->EndData();
  const int stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "POST"},
                                        {":scheme", "http"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             std::move(body1), nullptr);
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x4, 0));
  // The client can only send 65535 bytes of data, as the stream window has not
  // yet been increased.
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id, 16384, 0x0, 0)).Times(3);
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id, 16383, 0x0, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::HEADERS, SpdyFrameType::DATA,
                            SpdyFrameType::DATA, SpdyFrameType::DATA,
                            SpdyFrameType::DATA}));
  visitor.Clear();

  // Can't write any more due to flow control.
  EXPECT_FALSE(adapter->want_write());

  const std::string settings_frame =
      TestFrameSequence().Settings({{INITIAL_WINDOW_SIZE, 80000u}}).Serialize();
  // SETTINGS with INITIAL_STREAM_WINDOW
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{INITIAL_WINDOW_SIZE, 80000u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t settings_result = adapter->ProcessBytes(settings_frame);
  EXPECT_EQ(settings_frame.size(), static_cast<size_t>(settings_result));

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  // The client can write more after receiving the INITIAL_WINDOW_SIZE setting.
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id, 14465, 0x0, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::DATA}));
}

TEST(OgHttp2AdapterTest, InvalidInitialWindowSetting) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const uint32_t kTooLargeInitialWindow = 1u << 31;
  const std::string initial_frames =
      TestFrameSequence()
          .Settings({{INITIAL_WINDOW_SIZE, kTooLargeInitialWindow}})
          .Serialize();
  // Server preface (SETTINGS with INITIAL_STREAM_WINDOW)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor,
              OnInvalidFrame(
                  0, Http2VisitorInterface::InvalidFrameError::kFlowControl));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kFlowControlError));

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  // Session will want to write a GOAWAY.
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(GOAWAY, 0, _, 0x0,
                  static_cast<int>(Http2ErrorCode::FLOW_CONTROL_ERROR)));

  int64_t result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  serialized.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(serialized,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
  visitor.Clear();
}

TEST(OggHttp2AdapterClientTest, InitialWindowSettingCausesOverflow) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});
  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));
  int64_t write_result = adapter->Send();
  EXPECT_EQ(0, write_result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const uint32_t kLargeInitialWindow = (1u << 31) - 1;
  const std::string frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(stream_id, {{":status", "200"}}, /*fin=*/false)
          .WindowUpdate(stream_id, 65536u)
          .Settings({{INITIAL_WINDOW_SIZE, kLargeInitialWindow}})
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, HEADERS, 0x4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(stream_id));
  EXPECT_CALL(visitor, OnHeaderForStream(stream_id, ":status", "200"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(stream_id));

  EXPECT_CALL(visitor, OnFrameHeader(stream_id, 4, WINDOW_UPDATE, 0x0));
  EXPECT_CALL(visitor, OnWindowUpdate(stream_id, 65536));

  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{INITIAL_WINDOW_SIZE,
                                              kLargeInitialWindow}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  // The stream window update plus the SETTINGS frame with INITIAL_WINDOW_SIZE
  // pushes the stream's flow control window outside of the acceptable range.
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, stream_id, 4, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(RST_STREAM, stream_id, 4, 0x0,
                  static_cast<int>(Http2ErrorCode::FLOW_CONTROL_ERROR)));
  EXPECT_CALL(visitor,
              OnCloseStream(stream_id, Http2ErrorCode::HTTP2_NO_ERROR));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, FailureSendingConnectionPreface) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  visitor.set_has_write_error();
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kSendError));

  int result = adapter->Send();
  EXPECT_LT(result, 0);
}

TEST(OgHttp2AdapterTest, MaxFrameSizeSettingNotAppliedBeforeAck) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const uint32_t large_frame_size = kDefaultFramePayloadSizeLimit + 42;
  adapter->SubmitSettings({{MAX_FRAME_SIZE, large_frame_size}});
  const int32_t stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "GET"},
                                        {":scheme", "https"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             /*data_source=*/nullptr, /*user_data=*/nullptr);
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(adapter->want_write());

  testing::InSequence s;

  // Client preface (SETTINGS with MAX_FRAME_SIZE) and request HEADERS
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string server_frames =
      TestFrameSequence()
          .ServerPreface()
          .Headers(1, {{":status", "200"}}, /*fin=*/false)
          .Data(1, std::string(large_frame_size, 'a'))
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // Response HEADERS. Because the SETTINGS with MAX_FRAME_SIZE was not
  // acknowledged, the large DATA is treated as a connection error. Note that
  // oghttp2 delivers the DATA frame header and connection error events.
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, large_frame_size, DATA, 0x0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t process_result = adapter->ProcessBytes(server_frames);
  EXPECT_EQ(server_frames.size(), static_cast<size_t>(process_result));

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::FRAME_SIZE_ERROR)));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, MaxFrameSizeSettingAppliedAfterAck) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const uint32_t large_frame_size = kDefaultFramePayloadSizeLimit + 42;
  adapter->SubmitSettings({{MAX_FRAME_SIZE, large_frame_size}});
  const int32_t stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "GET"},
                                        {":scheme", "https"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             /*data_source=*/nullptr, /*user_data=*/nullptr);
  EXPECT_GT(stream_id, 0);
  EXPECT_TRUE(adapter->want_write());

  testing::InSequence s;

  // Client preface (SETTINGS with MAX_FRAME_SIZE) and request HEADERS
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data,
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string server_frames =
      TestFrameSequence()
          .ServerPreface()
          .SettingsAck()
          .Headers(1, {{":status", "200"}}, /*fin=*/false)
          .Data(1, std::string(large_frame_size, 'a'))
          .Serialize();

  // Server preface (empty SETTINGS) and ack of SETTINGS.
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());

  // Response HEADERS and DATA. Because the SETTINGS with MAX_FRAME_SIZE was
  // acknowledged, the large DATA is accepted without any error.
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":status", "200"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, large_frame_size, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, large_frame_size));
  EXPECT_CALL(visitor, OnDataForStream(1, _));

  const int64_t process_result = adapter->ProcessBytes(server_frames);
  EXPECT_EQ(server_frames.size(), static_cast<size_t>(process_result));

  // Client ack of SETTINGS.
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ClientForbidsPushPromise) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  int write_result = adapter->Send();
  EXPECT_EQ(0, write_result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));

  visitor.Clear();

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});
  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));
  write_result = adapter->Send();
  EXPECT_EQ(0, write_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::vector<Header> push_headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/push"}});
  const std::string frames = TestFrameSequence()
                                 .ServerPreface()
                                 .SettingsAck()
                                 .PushPromise(stream_id, 2, push_headers)
                                 .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // SETTINGS ack (to acknowledge PUSH_ENABLED=0, though this is not explicitly
  // required for OgHttp2: should it be?)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck);

  // The PUSH_PROMISE is treated as an invalid frame.
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, PUSH_PROMISE, _));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidPushPromise));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientForbidsPushStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  int write_result = adapter->Send();
  EXPECT_EQ(0, write_result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));

  visitor.Clear();

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});
  const int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  ASSERT_GT(stream_id, 0);
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));
  write_result = adapter->Send();
  EXPECT_EQ(0, write_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string frames =
      TestFrameSequence()
          .ServerPreface()
          .SettingsAck()
          .Headers(2,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/true)
          .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // SETTINGS ack (to acknowledge PUSH_ENABLED=0, though this is not explicitly
  // required for OgHttp2: should it be?)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck);

  // The push HEADERS are invalid.
  EXPECT_CALL(visitor, OnFrameHeader(2, _, HEADERS, _));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidNewStreamId));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientReceivesDataOnClosedStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view data = visitor.data();
  EXPECT_THAT(data, testing::StartsWith(spdy::kHttp2ConnectionHeaderPrefix));
  data.remove_prefix(strlen(spdy::kHttp2ConnectionHeaderPrefix));
  EXPECT_THAT(data, EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  const std::string initial_frames =
      TestFrameSequence().ServerPreface().Serialize();
  testing::InSequence s;

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(initial_result));

  // Client SETTINGS ack
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // Let the client open a stream with a request.
  int stream_id =
      adapter->SubmitRequest(ToHeaders({{":method", "GET"},
                                        {":scheme", "http"},
                                        {":authority", "example.com"},
                                        {":path", "/this/is/request/one"}}),
                             nullptr, nullptr);
  EXPECT_GT(stream_id, 0);

  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id, _, 0x5, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
  visitor.Clear();

  // Let the client RST_STREAM the stream it opened.
  adapter->SubmitRst(stream_id, Http2ErrorCode::CANCEL);
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, stream_id, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(RST_STREAM, stream_id, _, 0x0,
                                   static_cast<int>(Http2ErrorCode::CANCEL)));
  EXPECT_CALL(visitor,
              OnCloseStream(stream_id, Http2ErrorCode::HTTP2_NO_ERROR));

  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::RST_STREAM}));
  visitor.Clear();

  // Let the server send a response on the stream. (It might not have received
  // the RST_STREAM yet.)
  const std::string response_frames =
      TestFrameSequence()
          .Headers(stream_id,
                   {{":status", "200"},
                    {"server", "my-fake-server"},
                    {"date", "Tue, 6 Apr 2021 12:54:01 GMT"}},
                   /*fin=*/false)
          .Data(stream_id, "This is the response body.", /*fin=*/true)
          .Serialize();

  // The visitor gets notified about the HEADERS frame and DATA frame for the
  // closed stream with no further processing on either frame.
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, HEADERS, 0x4));
  EXPECT_CALL(visitor, OnFrameHeader(stream_id, _, DATA, 0x1));

  const int64_t response_result = adapter->ProcessBytes(response_frames);
  EXPECT_EQ(response_frames.size(), static_cast<size_t>(response_result));

  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, ClientEncountersFlowControlBlock) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const std::string kBody = std::string(100 * 1024, 'a');
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->AppendPayload(kBody);
  body1->EndData();

  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, std::move(body1), nullptr);
  ASSERT_GT(stream_id1, 0);

  const std::vector<Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});

  auto body2 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body2->AppendPayload(kBody);
  body2->EndData();

  const int32_t stream_id2 =
      adapter->SubmitRequest(headers2, std::move(body2), nullptr);
  ASSERT_GT(stream_id2, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id2, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id2, _, 0x4, 0));
  // 4 DATA frames should saturate the default 64kB stream/connection flow
  // control window.
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id1, _, 0x0, 0)).Times(4);

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_EQ(0, adapter->GetSendWindowSize());

  const std::string stream_frames = TestFrameSequence()
                                        .ServerPreface()
                                        .WindowUpdate(0, 80000)
                                        .WindowUpdate(stream_id1, 20000)
                                        .Serialize();

  // Server preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 80000));
  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(1, 20000));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id2, _, 0x0, 0))
      .Times(testing::AtLeast(1));
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id1, _, 0x0, 0))
      .Times(testing::AtLeast(1));

  EXPECT_TRUE(adapter->want_write());
  result = adapter->Send();
  EXPECT_EQ(0, result);
}

TEST(OgHttp2AdapterTest, ClientSendsTrailersAfterFlowControlBlock) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->AppendPayload("Really small body.");
  body1->EndData();

  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, std::move(body1), nullptr);
  ASSERT_GT(stream_id1, 0);

  const std::vector<Header> headers2 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/two"}});

  const std::string kBody = std::string(100 * 1024, 'a');
  auto body2 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body2->AppendPayload(kBody);
  body2->EndData();

  const int32_t stream_id2 =
      adapter->SubmitRequest(headers2, std::move(body2), nullptr);
  ASSERT_GT(stream_id2, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id2, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id2, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id1, _, 0x0, 0)).Times(1);
  // 4 DATA frames should saturate the default 64kB stream/connection flow
  // control window.
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id2, _, 0x0, 0)).Times(4);

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_FALSE(adapter->want_write());
  EXPECT_EQ(0, adapter->GetSendWindowSize());

  const std::vector<Header> trailers1 =
      ToHeaders({{"extra-info", "Trailers are weird but good?"}});
  adapter->SubmitTrailer(stream_id1, trailers1);

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
}

TEST(OgHttp2AdapterTest, ClientSendsMetadataAfterFlowControlBlock) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}});

  const std::string kBody = std::string(100 * 1024, 'a');
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->AppendPayload(kBody);
  body1->EndData();

  const int32_t stream_id1 =
      adapter->SubmitRequest(headers1, std::move(body1), nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_id1, _, 0x4, 0));
  // 4 DATA frames should saturate the default 64kB stream/connection flow
  // control window.
  EXPECT_CALL(visitor, OnFrameSent(DATA, stream_id1, _, 0x0, 0)).Times(4);

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_FALSE(adapter->want_write());
  EXPECT_EQ(0, adapter->GetSendWindowSize());

  auto source = absl::make_unique<TestMetadataSource>(ToHeaderBlock(ToHeaders(
      {{"query-cost", "is too darn high"}, {"secret-sauce", "hollandaise"}})));
  adapter->SubmitMetadata(1, 16384u, std::move(source));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x4, 0));

  result = adapter->Send();
  EXPECT_EQ(0, result);
}

TEST(OgHttp2AdapterTest, ClientQueuesRequests) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kClient;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  adapter->Send();

  const std::string initial_frames =
      TestFrameSequence()
          .ServerPreface({{MAX_CONCURRENT_STREAMS, 2}})
          .SettingsAck()
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0x0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{
                           Http2KnownSettingsId::MAX_CONCURRENT_STREAMS, 2u}));
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());

  adapter->ProcessBytes(initial_frames);

  const std::vector<Header> headers =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/example/request"}});
  std::vector<int32_t> stream_ids;
  // Start two, which hits the limit.
  int32_t stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  stream_ids.push_back(stream_id);
  stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  stream_ids.push_back(stream_id);
  // Start two more, which must be queued.
  stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  stream_ids.push_back(stream_id);
  stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  stream_ids.push_back(stream_id);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_ids[0], _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_ids[0], _, 0x5, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_ids[1], _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_ids[1], _, 0x5, 0));

  adapter->Send();

  const std::string update_streams =
      TestFrameSequence().Settings({{MAX_CONCURRENT_STREAMS, 5}}).Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0x0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSetting(Http2Setting{
                           Http2KnownSettingsId::MAX_CONCURRENT_STREAMS, 5u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  adapter->ProcessBytes(update_streams);
  stream_id = adapter->SubmitRequest(headers, nullptr, nullptr);
  stream_ids.push_back(stream_id);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_ids[2], _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_ids[2], _, 0x5, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_ids[3], _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_ids[3], _, 0x5, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, stream_ids[4], _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, stream_ids[4], _, 0x5, 0));
  // Header frames should all have been sent in order, regardless of any
  // queuing.

  adapter->Send();
}

TEST(OgHttp2AdapterTest, SubmitMetadata) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  auto source = absl::make_unique<TestMetadataSource>(ToHeaderBlock(ToHeaders(
      {{"query-cost", "is too darn high"}, {"secret-sauce", "hollandaise"}})));
  adapter->SubmitMetadata(1, 16384u, std::move(source));
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x4, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS,
                            static_cast<SpdyFrameType>(kMetadataFrameType)}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, SubmitMetadataMultipleFrames) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const auto kLargeValue = std::string(63 * 1024, 'a');
  auto source = absl::make_unique<TestMetadataSource>(
      ToHeaderBlock(ToHeaders({{"large-value", kLargeValue}})));
  adapter->SubmitMetadata(1, 16384u, std::move(source));
  EXPECT_TRUE(adapter->want_write());

  testing::InSequence seq;
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x4, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  absl::string_view serialized = visitor.data();
  EXPECT_THAT(serialized,
              EqualsFrames({SpdyFrameType::SETTINGS,
                            static_cast<SpdyFrameType>(kMetadataFrameType),
                            static_cast<SpdyFrameType>(kMetadataFrameType),
                            static_cast<SpdyFrameType>(kMetadataFrameType),
                            static_cast<SpdyFrameType>(kMetadataFrameType)}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, SubmitConnectionMetadata) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  auto source = absl::make_unique<TestMetadataSource>(ToHeaderBlock(ToHeaders(
      {{"query-cost", "is too darn high"}, {"secret-sauce", "hollandaise"}})));
  adapter->SubmitMetadata(0, 16384u, std::move(source));
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 0, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 0, _, 0x4, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS,
                            static_cast<SpdyFrameType>(kMetadataFrameType)}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, GetSendWindowSize) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const int peer_window = adapter->GetSendWindowSize();
  EXPECT_EQ(peer_window, kInitialFlowControlWindowSize);
}

TEST(OgHttp2AdapterTest, MarkDataConsumedForNonexistentStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  // Send some data on stream 1 so the connection window manager doesn't
  // underflow later.
  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Data(1, "Some data on stream 1")
                                 .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, _));
  EXPECT_CALL(visitor, OnDataForStream(1, _));

  adapter->ProcessBytes(frames);

  // This should not cause a crash or QUICHE_BUG.
  adapter->MarkDataConsumedForStream(3, 11);
}

TEST(OgHttp2AdapterTest, TestSerialize) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_TRUE(adapter->want_read());
  EXPECT_FALSE(adapter->want_write());

  adapter->SubmitSettings(
      {{HEADER_TABLE_SIZE, 128}, {MAX_FRAME_SIZE, 128 << 10}});
  EXPECT_TRUE(adapter->want_write());

  const Http2StreamId accepted_stream = 3;
  const Http2StreamId rejected_stream = 7;
  adapter->SubmitPriorityForStream(accepted_stream, 1, 255, true);
  adapter->SubmitRst(rejected_stream, Http2ErrorCode::CANCEL);
  adapter->SubmitPing(42);
  adapter->SubmitGoAway(13, Http2ErrorCode::HTTP2_NO_ERROR, "");
  adapter->SubmitWindowUpdate(accepted_stream, 127);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(PRIORITY, accepted_stream, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(PRIORITY, accepted_stream, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, rejected_stream, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(RST_STREAM, rejected_stream, _, 0x0, 0x8));
  EXPECT_CALL(visitor, OnBeforeFrameSent(PING, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(PING, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(GOAWAY, 0, _, 0x0, 0));
  EXPECT_CALL(visitor,
              OnBeforeFrameSent(WINDOW_UPDATE, accepted_stream, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(WINDOW_UPDATE, accepted_stream, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::PRIORITY,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::PING,
                    SpdyFrameType::GOAWAY, SpdyFrameType::WINDOW_UPDATE}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, TestPartialSerialize) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_FALSE(adapter->want_write());

  adapter->SubmitSettings(
      {{HEADER_TABLE_SIZE, 128}, {MAX_FRAME_SIZE, 128 << 10}});
  adapter->SubmitGoAway(13, Http2ErrorCode::HTTP2_NO_ERROR,
                        "And don't come back!");
  adapter->SubmitPing(42);
  EXPECT_TRUE(adapter->want_write());

  visitor.set_send_limit(20);
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(GOAWAY, 0, _, 0x0, 0));
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(PING, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(PING, 0, _, 0x0, 0));
  result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_FALSE(adapter->want_write());
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY,
                            SpdyFrameType::PING}));
}

TEST(OgHttp2AdapterTest, TestStreamInitialWindowSizeUpdates) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  adapter->SubmitSettings({{INITIAL_WINDOW_SIZE, 80000}});
  EXPECT_TRUE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  // New stream window size has not yet been applied.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(1), 65535);

  // Server initial SETTINGS
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  // SETTINGS ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  int result = adapter->Send();
  EXPECT_EQ(0, result);

  // New stream window size has still not been applied.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(1), 65535);

  const std::string ack = TestFrameSequence().SettingsAck().Serialize();
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());
  adapter->ProcessBytes(ack);

  // New stream window size has finally been applied upon SETTINGS ack.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(1), 80000);

  // Update the stream window size again.
  adapter->SubmitSettings({{INITIAL_WINDOW_SIZE, 90000}});
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  result = adapter->Send();
  EXPECT_EQ(0, result);

  // New stream window size has not yet been applied.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(1), 80000);

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());
  adapter->ProcessBytes(ack);

  // New stream window size is applied after the ack.
  EXPECT_EQ(adapter->GetStreamReceiveWindowSize(1), 90000);
}

TEST(OgHttp2AdapterTest, ConnectionErrorOnControlFrameSent) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface().Ping(42).Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // PING
  EXPECT_CALL(visitor, OnFrameHeader(0, _, PING, 0));
  EXPECT_CALL(visitor, OnPing(42, false));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  // Server preface (SETTINGS)
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  // SETTINGS ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0))
      .WillOnce(testing::Return(-902));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kSendError));

  int send_result = adapter->Send();
  EXPECT_LT(send_result, 0);

  EXPECT_FALSE(adapter->want_write());

  send_result = adapter->Send();
  EXPECT_LT(send_result, 0);
}

TEST(OgHttp2AdapterTest, ConnectionErrorOnDataFrameSent) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  auto body = absl::make_unique<TestDataFrameSource>(visitor, true);
  body->AppendPayload("Here is some data, which will lead to a fatal error");
  TestDataFrameSource* body_ptr = body.get();
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}}), std::move(body));
  ASSERT_EQ(0, submit_result);

  EXPECT_TRUE(adapter->want_write());

  // Server preface (SETTINGS)
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  // SETTINGS ack
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  // Stream 1, with doomed DATA
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, _, 0x0, 0))
      .WillOnce(testing::Return(-902));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kSendError));

  int send_result = adapter->Send();
  EXPECT_LT(send_result, 0);

  body_ptr->AppendPayload("After the fatal error, data will be sent no more");

  EXPECT_FALSE(adapter->want_write());

  send_result = adapter->Send();
  EXPECT_LT(send_result, 0);
}

TEST(OgHttp2AdapterTest, ClientSendsContinuation) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true,
                                          /*add_continuation=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 1));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, CONTINUATION, 4));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));
}

TEST(OgHttp2AdapterTest, ClientSendsMetadataWithContinuation) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames =
      TestFrameSequence()
          .ClientPreface()
          .Metadata(0, "Example connection metadata in multiple frames", true)
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/false,
                   /*add_continuation=*/true)
          .Metadata(1,
                    "Some stream metadata that's also sent in multiple frames",
                    true)
          .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Metadata on stream 0
  EXPECT_CALL(visitor, OnFrameHeader(0, _, kMetadataFrameType, 0));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnFrameHeader(0, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataForStream(0, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(0));

  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, CONTINUATION, 4));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  // Metadata on stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, kMetadataFrameType, 0));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, kMetadataFrameType, 4));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnMetadataForStream(1, _));
  EXPECT_CALL(visitor, OnMetadataEndForStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));
  EXPECT_EQ("Example connection metadata in multiple frames",
            absl::StrJoin(visitor.GetMetadata(0), ""));
  EXPECT_EQ("Some stream metadata that's also sent in multiple frames",
            absl::StrJoin(visitor.GetMetadata(1), ""));
}

TEST(OgHttp2AdapterTest, RepeatedHeaderNames) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"accept", "text/plain"},
                                           {"accept", "text/html"}},
                                          /*fin=*/true)
                                 .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "accept", "text/plain"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "accept", "text/html"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  const std::vector<Header> headers1 = ToHeaders(
      {{":status", "200"}, {"content-length", "10"}, {"content-length", "10"}});
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, true);
  body1->AppendPayload("perfection");
  body1->EndData();

  int submit_result = adapter->SubmitResponse(1, headers1, std::move(body1));
  ASSERT_EQ(0, submit_result);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, 10, 0x1, 0));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS, SpdyFrameType::DATA}));
}

TEST(OgHttp2AdapterTest, ServerRespondsToRequestWithTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1, {{":method", "GET"},
                       {":scheme", "https"},
                       {":authority", "example.com"},
                       {":path", "/this/is/request/one"}})
          .Data(1, "Example data, woohoo.")
          .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, _));
  EXPECT_CALL(visitor, OnDataForStream(1, _));

  int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  const std::vector<Header> headers1 = ToHeaders({{":status", "200"}});
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, true);
  TestDataFrameSource* body1_ptr = body1.get();

  int submit_result = adapter->SubmitResponse(1, headers1, std::move(body1));
  ASSERT_EQ(0, submit_result);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS}));
  visitor.Clear();

  const std::string more_frames =
      TestFrameSequence()
          .Headers(1, {{"extra-info", "Trailers are weird but good?"}},
                   /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "extra-info",
                                         "Trailers are weird but good?"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  result = adapter->ProcessBytes(more_frames);
  EXPECT_EQ(more_frames.size(), static_cast<size_t>(result));

  body1_ptr->EndData();
  EXPECT_EQ(true, adapter->ResumeStream(1));

  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::DATA}));
}

TEST(OgHttp2AdapterTest, ServerReceivesMoreHeaderBytesThanConfigured) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.max_header_list_bytes = 42;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"},
                    {"from-douglas-de-fermat",
                     "I have discovered a truly marvelous answer to the life, "
                     "the universe, and everything that the header setting is "
                     "too narrow to contain."}},
                   /*fin=*/true)
          .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(result), frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::COMPRESSION_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerSubmitsResponseWithDataSourceError) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->SimulateError();
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
      std::move(body1));
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
  // TODO(birenroy): Send RST_STREAM INTERNAL_ERROR to the client as well.
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::INTERNAL_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS}));
  visitor.Clear();
  EXPECT_FALSE(adapter->want_write());

  // Since the stream has been closed, it is not possible to submit trailers for
  // the stream.
  int trailer_result =
      adapter->SubmitTrailer(1, ToHeaders({{":final-status", "a-ok"}}));
  ASSERT_LT(trailer_result, 0);
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, CompleteRequestWithServerResponse) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/false)
          .Data(1, "This is the response body.", /*fin=*/true)
          .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, _));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  int submit_result =
      adapter->SubmitResponse(1, ToHeaders({{":status", "200"}}), nullptr);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, IncompleteRequestWithServerResponse) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  int submit_result =
      adapter->SubmitResponse(1, ToHeaders({{":status", "200"}}), nullptr);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));
  // RST_STREAM NO_ERROR option is disabled.

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, IncompleteRequestWithServerResponseRstStreamEnabled) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.rst_stream_no_error_when_incomplete = true;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  int submit_result =
      adapter->SubmitResponse(1, ToHeaders({{":status", "200"}}), nullptr);
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(RST_STREAM, 1, 4, 0x0, 0));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                    SpdyFrameType::HEADERS, SpdyFrameType::RST_STREAM}));
  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, ServerHandlesMultipleContentLength) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/1"},
                                           {"content-length", "7"},
                                           {"content-length", "7"}},
                                          /*fin=*/false)
                                 .Headers(3,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/3"},
                                           {"content-length", "11"},
                                           {"content-length", "13"}},
                                          /*fin=*/false)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/1"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "content-length", "7"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  // Stream 3
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":path", "/3"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, "content-length", "11"));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(3, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));
}

TEST(OgHttp2AdapterTest, ServerSendsInvalidTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  const absl::string_view kBody = "This is an example response body.";

  // The body source must indicate that the end of the body is not the end of
  // the stream.
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->AppendPayload(kBody);
  body1->EndData();
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
      std::move(body1));
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, _, 0x0, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS, SpdyFrameType::DATA}));
  EXPECT_THAT(visitor.data(), testing::HasSubstr(kBody));
  visitor.Clear();
  EXPECT_FALSE(adapter->want_write());

  // The body source has been exhausted by the call to Send() above.
  int trailer_result =
      adapter->SubmitTrailer(1, ToHeaders({{":final-status", "a-ok"}}));
  ASSERT_EQ(trailer_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
}

TEST(OgHttp2AdapterTest, ServerQueuesMetadataThenTrailers) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  EXPECT_FALSE(adapter->want_write());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  const absl::string_view kBody = "This is an example response body.";

  // The body source must indicate that the end of the body is not the end of
  // the stream.
  auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
  body1->AppendPayload(kBody);
  body1->EndData();
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
      std::move(body1));
  EXPECT_EQ(submit_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, _, 0x0, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS, SpdyFrameType::DATA}));
  EXPECT_THAT(visitor.data(), testing::HasSubstr(kBody));
  visitor.Clear();
  EXPECT_FALSE(adapter->want_write());

  spdy::SpdyHeaderBlock block;
  block["key"] = "wild value!";
  adapter->SubmitMetadata(
      1, 16384u, absl::make_unique<TestMetadataSource>(std::move(block)));

  int trailer_result =
      adapter->SubmitTrailer(1, ToHeaders({{":final-status", "a-ok"}}));
  ASSERT_EQ(trailer_result, 0);
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(kMetadataFrameType, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(kMetadataFrameType, 1, _, 0x4, 0));

  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));

  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({static_cast<SpdyFrameType>(kMetadataFrameType),
                            SpdyFrameType::HEADERS}));
}

TEST(OgHttp2AdapterTest, ServerHandlesDataWithPadding) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Data(1, "This is the request body.",
                                       /*fin=*/true, /*padding_length=*/39)
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "http"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 25 + 39, DATA, 0x9));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 25 + 39));
  // Note: oghttp2 passes padding information before the actual data.
  EXPECT_CALL(visitor, OnDataPaddingLength(1, 39));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the request body."));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnEndStream(3));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<int64_t>(frames.size()), result);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ServerHandlesHostHeader) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":path", "/this/is/request/one"},
                                           {"host", "example.com"}},
                                          /*fin=*/true)
                                 .Headers(3,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"host", "example.com"}},
                                          /*fin=*/true)
                                 .Headers(5,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "foo.com"},
                                           {":path", "/this/is/request/one"},
                                           {"host", "bar.com"}},
                                          /*fin=*/true)
                                 .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnEndStream(3));

  EXPECT_CALL(visitor, OnFrameHeader(5, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(5));
  EXPECT_CALL(visitor, OnHeaderForStream(5, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(5, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 5, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 5, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(5, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  visitor.Clear();
}

// Tests the case where the response body is in the progress of being sent while
// trailers are queued.
TEST(OgHttp2AdapterTest, ServerSubmitsTrailersWhileDataDeferred) {
  DataSavingVisitor visitor;
  for (const bool queue_trailers : {true, false}) {
    OgHttp2Adapter::Options options;
    options.perspective = Perspective::kServer;
    options.trailers_require_end_data = queue_trailers;
    auto adapter = OgHttp2Adapter::Create(visitor, options);

    const std::string frames = TestFrameSequence()
                                   .ClientPreface()
                                   .Headers(1,
                                            {{":method", "POST"},
                                             {":scheme", "https"},
                                             {":authority", "example.com"},
                                             {":path", "/this/is/request/one"}},
                                            /*fin=*/false)
                                   .WindowUpdate(1, 2000)
                                   .Data(1, "This is the request body.")
                                   .WindowUpdate(0, 2000)
                                   .Serialize();
    testing::InSequence s;

    // Client preface (empty SETTINGS)
    EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
    EXPECT_CALL(visitor, OnSettingsStart());
    EXPECT_CALL(visitor, OnSettingsEnd());

    EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
    EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
    EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
    EXPECT_CALL(visitor, OnEndHeadersForStream(1));
    EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
    EXPECT_CALL(visitor, OnWindowUpdate(1, 2000));
    EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0));
    EXPECT_CALL(visitor, OnBeginDataForStream(1, _));
    EXPECT_CALL(visitor, OnDataForStream(1, "This is the request body."));
    EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
    EXPECT_CALL(visitor, OnWindowUpdate(0, 2000));

    const int64_t result = adapter->ProcessBytes(frames);
    EXPECT_EQ(frames.size(), static_cast<size_t>(result));

    EXPECT_TRUE(adapter->want_write());

    EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
    EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
    EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
    EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

    int send_result = adapter->Send();
    EXPECT_EQ(0, send_result);
    visitor.Clear();

    const absl::string_view kBody = "This is an example response body.";

    // The body source must indicate that the end of the body is not the end of
    // the stream.
    auto body1 = absl::make_unique<TestDataFrameSource>(visitor, false);
    body1->AppendPayload(kBody);
    auto* body1_ptr = body1.get();
    int submit_result = adapter->SubmitResponse(
        1, ToHeaders({{":status", "200"}, {"x-comment", "Sure, sounds good."}}),
        std::move(body1));
    EXPECT_EQ(submit_result, 0);
    EXPECT_TRUE(adapter->want_write());

    EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
    EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
    EXPECT_CALL(visitor, OnFrameSent(DATA, 1, _, 0x0, 0));

    send_result = adapter->Send();
    EXPECT_EQ(0, send_result);
    visitor.Clear();
    EXPECT_FALSE(adapter->want_write());

    int trailer_result =
        adapter->SubmitTrailer(1, ToHeaders({{"final-status", "a-ok"}}));
    ASSERT_EQ(trailer_result, 0);
    if (queue_trailers) {
      // Even though there are new trailers to write, the data source has not
      // finished writing data and is blocked.
      EXPECT_FALSE(adapter->want_write());

      body1_ptr->EndData();
      adapter->ResumeStream(1);
      EXPECT_TRUE(adapter->want_write());

      EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
      EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));

      send_result = adapter->Send();
      EXPECT_EQ(0, send_result);
    } else {
      // Even though the data source has not finished sending data, the library
      // will write the trailers anyway.
      EXPECT_TRUE(adapter->want_write());

      EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
      EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));

      send_result = adapter->Send();
      EXPECT_EQ(0, send_result);
      EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::HEADERS}));
    }
  }
}

TEST(OgHttp2AdapterTest, ClientDisobeysConnectionFlowControl) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"accept", "some bogus value!"}},
                                          /*fin=*/false)
                                 // 70000 bytes of data
                                 .Data(1, std::string(16384, 'a'))
                                 .Data(1, std::string(16384, 'a'))
                                 .Data(1, std::string(16384, 'a'))
                                 .Data(1, std::string(16384, 'a'))
                                 .Data(1, std::string(4464, 'a'))
                                 .Serialize();

  testing::InSequence s;
  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 16384));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 16384));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 16384));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kFlowControlError));
  // No further frame data or headers are delivered.

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(GOAWAY, 0, _, 0x0,
                  static_cast<int>(Http2ErrorCode::FLOW_CONTROL_ERROR)));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientDisobeysConnectionFlowControlWithOneDataFrame) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  // Allow the client to send a DATA frame that exceeds the connection flow
  // control window.
  const uint32_t window_overflow_bytes = kInitialFlowControlWindowSize + 1;
  adapter->SubmitSettings({{MAX_FRAME_SIZE, window_overflow_bytes}});

  const std::string initial_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":method", "POST"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/false)
          .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  int64_t process_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(process_result));

  EXPECT_TRUE(adapter->want_write());

  // Outbound SETTINGS containing MAX_FRAME_SIZE.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));

  // Ack of client's initial settings.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // Now let the client ack the MAX_FRAME_SIZE SETTINGS and send a DATA frame to
  // overflow the connection-level window. The result should be a GOAWAY.
  const std::string overflow_frames =
      TestFrameSequence()
          .SettingsAck()
          .Data(1, std::string(window_overflow_bytes, 'a'))
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());
  EXPECT_CALL(visitor, OnFrameHeader(1, window_overflow_bytes, DATA, 0x0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kFlowControlError));
  // No further frame data is delivered.

  process_result = adapter->ProcessBytes(overflow_frames);
  EXPECT_EQ(overflow_frames.size(), static_cast<size_t>(process_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(GOAWAY, 0, _, 0x0,
                  static_cast<int>(Http2ErrorCode::FLOW_CONTROL_ERROR)));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientDisobeysConnectionFlowControlAcrossReads) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  // Allow the client to send a DATA frame that exceeds the connection flow
  // control window.
  const uint32_t window_overflow_bytes = kInitialFlowControlWindowSize + 1;
  adapter->SubmitSettings({{MAX_FRAME_SIZE, window_overflow_bytes}});

  const std::string initial_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":method", "POST"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/false)
          .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  int64_t process_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(initial_frames.size(), static_cast<size_t>(process_result));

  EXPECT_TRUE(adapter->want_write());

  // Outbound SETTINGS containing MAX_FRAME_SIZE.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));

  // Ack of client's initial settings.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // Now let the client ack the MAX_FRAME_SIZE SETTINGS and send a DATA frame to
  // overflow the connection-level window. The result should be a GOAWAY.
  const std::string overflow_frames =
      TestFrameSequence()
          .SettingsAck()
          .Data(1, std::string(window_overflow_bytes, 'a'))
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());
  EXPECT_CALL(visitor, OnFrameHeader(1, window_overflow_bytes, DATA, 0x0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kFlowControlError));

  const size_t chunk_length = 16384;
  ASSERT_GE(overflow_frames.size(), chunk_length);
  process_result =
      adapter->ProcessBytes(overflow_frames.substr(0, chunk_length));
  EXPECT_EQ(chunk_length, static_cast<size_t>(process_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(GOAWAY, 0, _, 0x0,
                  static_cast<int>(Http2ErrorCode::FLOW_CONTROL_ERROR)));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ClientDisobeysStreamFlowControl) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"accept", "some bogus value!"}},
                                          /*fin=*/false)
                                 .Serialize();
  const std::string more_frames = TestFrameSequence()
                                      // 70000 bytes of data
                                      .Data(1, std::string(16384, 'a'))
                                      .Data(1, std::string(16384, 'a'))
                                      .Data(1, std::string(16384, 'a'))
                                      .Data(1, std::string(16384, 'a'))
                                      .Data(1, std::string(4464, 'a'))
                                      .Serialize();

  testing::InSequence s;
  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));

  int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  adapter->SubmitWindowUpdate(0, 20000);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(WINDOW_UPDATE, 0, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(WINDOW_UPDATE, 0, 4, 0x0, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::WINDOW_UPDATE}));
  visitor.Clear();

  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 16384));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 16384));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 16384));
  EXPECT_CALL(visitor, OnDataForStream(1, _));
  EXPECT_CALL(visitor, OnFrameHeader(1, 16384, DATA, 0x0));
  // No further frame data or headers are delivered.

  result = adapter->ProcessBytes(more_frames);
  EXPECT_EQ(more_frames.size(), static_cast<size_t>(result));

  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(
      visitor,
      OnFrameSent(RST_STREAM, 1, 4, 0x0,
                  static_cast<int>(Http2ErrorCode::FLOW_CONTROL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerErrorWhileHandlingHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"accept", "some bogus value!"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .WindowUpdate(0, 2000)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "accept", "some bogus value!"))
      .WillOnce(testing::Return(Http2VisitorInterface::HEADER_RST_STREAM));
  // Stream WINDOW_UPDATE and DATA frames are not delivered to the visitor.
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 2000));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  // SETTINGS ack
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerErrorWhileHandlingHeadersDropsFrames) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"accept", "some bogus value!"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .Metadata(1, "This is the request metadata.")
                                 .RstStream(1, Http2ErrorCode::CANCEL)
                                 .WindowUpdate(0, 2000)
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/false)
                                 .Metadata(3, "This is the request metadata.",
                                           /*multiple_frames=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnHeaderForStream(1, "accept", "some bogus value!"))
      .WillOnce(testing::Return(Http2VisitorInterface::HEADER_RST_STREAM));
  // Frames for the RST_STREAM-marked stream are not delivered to the visitor.
  // Note: nghttp2 still delivers control frames and metadata for the stream.
  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 2000));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, kMetadataFrameType, 0));
  EXPECT_CALL(visitor, OnBeginMetadataForStream(3, _));
  EXPECT_CALL(visitor, OnMetadataForStream(3, "This is the re"))
      .WillOnce(testing::DoAll(testing::InvokeWithoutArgs([&adapter]() {
                                 adapter->SubmitRst(
                                     3, Http2ErrorCode::REFUSED_STREAM);
                               }),
                               testing::Return(true)));
  // The rest of the metadata is not delivered to the visitor.

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::REFUSED_STREAM)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  // SETTINGS ack
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerConnectionErrorWhileHandlingHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"Accept", "uppercase, oh boy!"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .WindowUpdate(0, 2000)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader))
      .WillOnce(testing::Return(false));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kHeaderError));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_LT(result, 0);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::RST_STREAM,
                            SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerErrorAfterHandlingHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .WindowUpdate(0, 2000)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1))
      .WillOnce(testing::Return(false));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_LT(result, 0);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

// Exercises the case when a visitor chooses to reject a frame based solely on
// the frame header, which is a fatal error for the connection.
TEST(OgHttp2AdapterTest, ServerRejectsFrameHeader) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Ping(64)
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .WindowUpdate(1, 2000)
                                 .Data(1, "This is the request body.")
                                 .WindowUpdate(0, 2000)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 8, PING, 0))
      .WillOnce(testing::Return(false));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_LT(result, 0);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerRejectsBeginningOfData) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Data(1, "This is the request body.")
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "http"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .RstStream(3, Http2ErrorCode::CANCEL)
                                 .Ping(47)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 25, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 25))
      .WillOnce(testing::Return(false));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_LT(result, 0);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerReceivesTooLargeHeader) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.max_header_list_bytes = 64 * 1024;
  options.max_header_field_size = 64 * 1024;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  // Due to configuration, the library will accept a maximum of 64kB of huffman
  // encoded data per header field.
  const std::string too_large_value = std::string(80 * 1024, 'q');
  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"},
                                           {"x-toobig", too_large_value}},
                                          /*fin=*/true)
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, END_STREAM));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, CONTINUATION, 0)).Times(3);
  EXPECT_CALL(visitor, OnFrameHeader(1, _, CONTINUATION, END_HEADERS));
  // Further header processing is skipped, as the header field is too large.

  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, END_STREAM | END_HEADERS));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnEndStream(3));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<int64_t>(frames.size()), result);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, ACK));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, ACK, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerReceivesInvalidAuthority) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "ex|ample.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<int64_t>(frames.size()), result);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0x0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0x0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, 4, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, 4, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttpAdapterTest, ServerReceivesGoAway) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .GoAway(0, Http2ErrorCode::HTTP2_NO_ERROR, "")
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(0, _, GOAWAY, 0x0));
  EXPECT_CALL(visitor, OnGoAway(0, Http2ErrorCode::HTTP2_NO_ERROR, ""));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<int64_t>(frames.size()), result);

  // The server should still be able to send a response after receiving a GOAWAY
  // with a lower last-stream-ID field, as the stream was client-initiated.
  const int submit_result =
      adapter->SubmitResponse(1, ToHeaders({{":status", "200"}}),
                              /*data_source=*/nullptr);
  ASSERT_EQ(0, submit_result);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0x0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0x0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x5));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x5, 0));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::HEADERS}));
}

TEST(OgHttp2AdapterTest, ServerRejectsStreamData) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Data(1, "This is the request body.")
                                 .Headers(3,
                                          {{":method", "GET"},
                                           {":scheme", "http"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .RstStream(3, Http2ErrorCode::CANCEL)
                                 .Ping(47)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, 25, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 25));
  EXPECT_CALL(visitor, OnDataForStream(1, _)).WillOnce(testing::Return(false));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kParseError));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_LT(result, 0);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

// Exercises a naive mutually recursive test client and server. This test fails
// without recursion guards in OgHttp2Session.
TEST(OgHttp2AdapterInteractionTest, ClientServerInteractionTest) {
  testing::NiceMock<MockHttp2Visitor> client_visitor;
  OgHttp2Adapter::Options client_options;
  client_options.perspective = Perspective::kClient;
  auto client_adapter = OgHttp2Adapter::Create(client_visitor, client_options);
  testing::NiceMock<MockHttp2Visitor> server_visitor;
  OgHttp2Adapter::Options server_options;
  server_options.perspective = Perspective::kServer;
  auto server_adapter = OgHttp2Adapter::Create(server_visitor, server_options);

  // Feeds bytes sent from the client into the server's ProcessBytes.
  EXPECT_CALL(client_visitor, OnReadyToSend(_))
      .WillRepeatedly(
          testing::Invoke(server_adapter.get(), &OgHttp2Adapter::ProcessBytes));
  // Feeds bytes sent from the server into the client's ProcessBytes.
  EXPECT_CALL(server_visitor, OnReadyToSend(_))
      .WillRepeatedly(
          testing::Invoke(client_adapter.get(), &OgHttp2Adapter::ProcessBytes));
  // Sets up the server to respond automatically to a request from a client.
  EXPECT_CALL(server_visitor, OnEndHeadersForStream(_))
      .WillRepeatedly([&server_adapter](Http2StreamId stream_id) {
        server_adapter->SubmitResponse(
            stream_id, ToHeaders({{":status", "200"}}), nullptr);
        server_adapter->Send();
        return true;
      });
  // Sets up the client to create a new stream automatically when receiving a
  // response.
  EXPECT_CALL(client_visitor, OnEndHeadersForStream(_))
      .WillRepeatedly([&client_adapter,
                       &client_visitor](Http2StreamId stream_id) {
        if (stream_id < 10) {
          const Http2StreamId new_stream_id = stream_id + 2;
          auto body =
              absl::make_unique<TestDataFrameSource>(client_visitor, true);
          body->AppendPayload("This is an example request body.");
          body->EndData();
          const int created_stream_id = client_adapter->SubmitRequest(
              ToHeaders({{":method", "GET"},
                         {":scheme", "http"},
                         {":authority", "example.com"},
                         {":path",
                          absl::StrCat("/this/is/request/", new_stream_id)}}),
              std::move(body), nullptr);
          EXPECT_EQ(new_stream_id, created_stream_id);
          client_adapter->Send();
        }
        return true;
      });

  // Submit a request to ensure the first stream is created.
  int stream_id = client_adapter->SubmitRequest(
      ToHeaders({{":method", "POST"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"}}),
      nullptr, nullptr);
  EXPECT_EQ(stream_id, 1);

  client_adapter->Send();
}

TEST(OgHttp2AdapterInteractionTest,
     ClientServerInteractionRepeatedHeaderNames) {
  DataSavingVisitor client_visitor;
  OgHttp2Adapter::Options client_options;
  client_options.perspective = Perspective::kClient;
  auto client_adapter = OgHttp2Adapter::Create(client_visitor, client_options);

  const std::vector<Header> headers1 =
      ToHeaders({{":method", "GET"},
                 {":scheme", "http"},
                 {":authority", "example.com"},
                 {":path", "/this/is/request/one"},
                 {"accept", "text/plain"},
                 {"accept", "text/html"}});

  const int32_t stream_id1 =
      client_adapter->SubmitRequest(headers1, nullptr, nullptr);
  ASSERT_GT(stream_id1, 0);

  EXPECT_CALL(client_visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(client_visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(client_visitor, OnBeforeFrameSent(HEADERS, stream_id1, _, 0x5));
  EXPECT_CALL(client_visitor, OnFrameSent(HEADERS, stream_id1, _, 0x5, 0));
  int send_result = client_adapter->Send();
  EXPECT_EQ(0, send_result);

  DataSavingVisitor server_visitor;
  OgHttp2Adapter::Options server_options;
  server_options.perspective = Perspective::kServer;
  auto server_adapter = OgHttp2Adapter::Create(server_visitor, server_options);

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(server_visitor, OnFrameHeader(0, _, SETTINGS, 0));
  EXPECT_CALL(server_visitor, OnSettingsStart());
  EXPECT_CALL(server_visitor, OnSetting).Times(testing::AnyNumber());
  EXPECT_CALL(server_visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(server_visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(server_visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(server_visitor, OnHeaderForStream(1, ":method", "GET"));
  EXPECT_CALL(server_visitor, OnHeaderForStream(1, ":scheme", "http"));
  EXPECT_CALL(server_visitor,
              OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(server_visitor,
              OnHeaderForStream(1, ":path", "/this/is/request/one"));
  EXPECT_CALL(server_visitor, OnHeaderForStream(1, "accept", "text/plain"));
  EXPECT_CALL(server_visitor, OnHeaderForStream(1, "accept", "text/html"));
  EXPECT_CALL(server_visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(server_visitor, OnEndStream(1));

  int64_t result = server_adapter->ProcessBytes(client_visitor.data());
  EXPECT_EQ(client_visitor.data().size(), static_cast<size_t>(result));
}

TEST(OgHttp2AdapterTest, ServerForbidsNewStreamBelowWatermark) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_EQ(0, adapter->GetHighestReceivedStreamId());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(3,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Data(3, "This is the request body.")
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "http"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/two"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":path", "/this/is/request/one"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnFrameHeader(3, 25, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(3, 25));
  EXPECT_CALL(visitor, OnDataForStream(3, "This is the request body."));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kInvalidNewStreamId));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(result), frames.size());

  EXPECT_EQ(3, adapter->GetHighestReceivedStreamId());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerForbidsWindowUpdateOnIdleStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_EQ(0, adapter->GetHighestReceivedStreamId());

  const std::string frames =
      TestFrameSequence().ClientPreface().WindowUpdate(1, 42).Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kWrongFrameSequence));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(result), frames.size());

  EXPECT_EQ(1, adapter->GetHighestReceivedStreamId());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerForbidsDataOnIdleStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_EQ(0, adapter->GetHighestReceivedStreamId());

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Data(1, "Sorry, out of order")
                                 .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kWrongFrameSequence));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(result), frames.size());

  EXPECT_EQ(1, adapter->GetHighestReceivedStreamId());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerForbidsRstStreamOnIdleStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_EQ(0, adapter->GetHighestReceivedStreamId());

  const std::string frames =
      TestFrameSequence()
          .ClientPreface()
          .RstStream(1, Http2ErrorCode::ENHANCE_YOUR_CALM)
          .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, RST_STREAM, 0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kWrongFrameSequence));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(result), frames.size());

  EXPECT_EQ(1, adapter->GetHighestReceivedStreamId());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerForbidsNewStreamAboveStreamLimit) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  adapter->SubmitSettings({{MAX_CONCURRENT_STREAMS, 1}});

  const std::string initial_frames =
      TestFrameSequence().ClientPreface().Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(static_cast<size_t>(initial_result), initial_frames.size());

  EXPECT_TRUE(adapter->want_write());

  // Server initial SETTINGS (with MAX_CONCURRENT_STREAMS) and SETTINGS ack.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // Let the client send a SETTINGS ack and then attempt to open more than the
  // advertised number of streams. The overflow stream should be rejected.
  const std::string stream_frames =
      TestFrameSequence()
          .SettingsAck()
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/true)
          .Headers(3,
                   {{":method", "GET"},
                    {":scheme", "http"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/two"}},
                   /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 0x5));
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(3, Http2VisitorInterface::InvalidFrameError::kProtocol));
  // The oghttp2 stack also signals the error via OnConnectionError().
  EXPECT_CALL(visitor, OnConnectionError(
                           ConnectionError::kExceededMaxConcurrentStreams));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(static_cast<size_t>(stream_result), stream_frames.size());

  // The server should send a GOAWAY for this error, even though
  // OnInvalidFrame() returns true.
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerRstStreamsNewStreamAboveStreamLimitBeforeAck) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  adapter->SubmitSettings({{MAX_CONCURRENT_STREAMS, 1}});

  const std::string initial_frames =
      TestFrameSequence().ClientPreface().Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(static_cast<size_t>(initial_result), initial_frames.size());

  EXPECT_TRUE(adapter->want_write());

  // Server initial SETTINGS (with MAX_CONCURRENT_STREAMS) and SETTINGS ack.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // Let the client avoid sending a SETTINGS ack and attempt to open more than
  // the advertised number of streams. The server should still reject the
  // overflow stream, albeit with RST_STREAM REFUSED_STREAM instead of GOAWAY.
  const std::string stream_frames =
      TestFrameSequence()
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/true)
          .Headers(3,
                   {{":method", "GET"},
                    {":scheme", "http"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/two"}},
                   /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 0x5));
  EXPECT_CALL(visitor,
              OnInvalidFrame(
                  3, Http2VisitorInterface::InvalidFrameError::kRefusedStream));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(static_cast<size_t>(stream_result), stream_frames.size());

  // The server sends a RST_STREAM for the offending stream.
  EXPECT_TRUE(adapter->want_write());
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::REFUSED_STREAM)));

  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), EqualsFrames({SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerForbidsProtocolPseudoheaderBeforeAck) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.allow_extended_connect = false;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string initial_frames =
      TestFrameSequence().ClientPreface().Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(static_cast<size_t>(initial_result), initial_frames.size());

  // The client attempts to send a CONNECT request with the `:protocol`
  // pseudoheader before receiving the server's SETTINGS frame.
  const std::string stream1_frames =
      TestFrameSequence()
          .Headers(1,
                   {{":method", "CONNECT"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"},
                    {":protocol", "websocket"}},
                   /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(5);
  EXPECT_CALL(visitor,
              OnInvalidFrame(
                  1, Http2VisitorInterface::InvalidFrameError::kHttpMessaging));

  int64_t stream_result = adapter->ProcessBytes(stream1_frames);
  EXPECT_EQ(static_cast<size_t>(stream_result), stream1_frames.size());

  // Server initial SETTINGS and SETTINGS ack.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  // The server sends a RST_STREAM for the offending stream.
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  // Server settings with ENABLE_CONNECT_PROTOCOL.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));

  adapter->SubmitSettings({{ENABLE_CONNECT_PROTOCOL, 1}});
  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::SETTINGS}));
  visitor.Clear();

  // The client attempts to send a CONNECT request with the `:protocol`
  // pseudoheader before acking the server's SETTINGS frame.
  const std::string stream3_frames =
      TestFrameSequence()
          .Headers(3,
                   {{":method", "CONNECT"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/two"},
                    {":protocol", "websocket"}},
                   /*fin=*/true)
          .Serialize();

  // After sending SETTINGS with `ENABLE_CONNECT_PROTOCOL`, oghttp2 matches
  // nghttp2 in allowing this, even though the `allow_extended_connect` option
  // is false.
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnEndStream(3));

  stream_result = adapter->ProcessBytes(stream3_frames);
  EXPECT_EQ(static_cast<size_t>(stream_result), stream3_frames.size());

  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, ServerAllowsProtocolPseudoheaderAfterAck) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);
  adapter->SubmitSettings({{ENABLE_CONNECT_PROTOCOL, 1}});

  const std::string initial_frames =
      TestFrameSequence().ClientPreface().Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(static_cast<size_t>(initial_result), initial_frames.size());

  // Server initial SETTINGS (with ENABLE_CONNECT_PROTOCOL) and SETTINGS ack.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  visitor.Clear();

  // The client attempts to send a CONNECT request with the `:protocol`
  // pseudoheader after acking the server's SETTINGS frame.
  const std::string stream_frames =
      TestFrameSequence()
          .SettingsAck()
          .Headers(1,
                   {{":method", "CONNECT"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"},
                    {":protocol", "websocket"}},
                   /*fin=*/true)
          .Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(0, _, SETTINGS, 0x1));
  EXPECT_CALL(visitor, OnSettingsAck());
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(static_cast<size_t>(stream_result), stream_frames.size());

  EXPECT_FALSE(adapter->want_write());
}

TEST(OgHttp2AdapterTest, SkipsSendingFramesForRejectedStream) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string initial_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "http"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/true)
          .Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t initial_result = adapter->ProcessBytes(initial_frames);
  EXPECT_EQ(static_cast<size_t>(initial_result), initial_frames.size());

  auto body = absl::make_unique<TestDataFrameSource>(visitor, true);
  body->AppendPayload("Here is some data, which will be completely ignored!");

  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}}), std::move(body));
  ASSERT_EQ(0, submit_result);

  auto source = absl::make_unique<TestMetadataSource>(ToHeaderBlock(ToHeaders(
      {{"query-cost", "is too darn high"}, {"secret-sauce", "hollandaise"}})));
  adapter->SubmitMetadata(1, 16384u, std::move(source));

  adapter->SubmitWindowUpdate(1, 1024);
  adapter->SubmitRst(1, Http2ErrorCode::INTERNAL_ERROR);

  // Server initial SETTINGS and SETTINGS ack.
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));

  // The server sends a RST_STREAM for the offending stream.
  // The response HEADERS, DATA and WINDOW_UPDATE are all ignored.
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::INTERNAL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttpAdapterServerTest, ServerStartsShutdown) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_FALSE(adapter->want_write());

  adapter->SubmitShutdownNotice();
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(GOAWAY, 0, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
}

TEST(OgHttp2AdapterTest, ServerStartsShutdownAfterGoaway) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  EXPECT_FALSE(adapter->want_write());

  adapter->SubmitGoAway(1, Http2ErrorCode::HTTP2_NO_ERROR,
                        "and don't come back!");
  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(GOAWAY, 0, _, 0x0, 0));

  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));

  // No-op, since a GOAWAY has previously been enqueued.
  adapter->SubmitShutdownNotice();
  EXPECT_FALSE(adapter->want_write());
}

// Verifies that a connection-level processing error results in repeatedly
// returning a positive value for ProcessBytes() to mark all data as consumed
// when the blackhole option is enabled.
TEST(OgHttp2AdapterTest, ConnectionErrorWithBlackholingData) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.blackhole_data_on_connection_error = true;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface().WindowUpdate(1, 42).Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kWrongFrameSequence));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(result), frames.size());

  // Ask the connection to process more bytes. Because the option is enabled,
  // the data should be marked as consumed.
  const std::string next_frame = TestFrameSequence().Ping(42).Serialize();
  const int64_t next_result = adapter->ProcessBytes(next_frame);
  EXPECT_EQ(static_cast<size_t>(next_result), next_frame.size());
}

// Verifies that a connection-level processing error results in returning a
// negative value for ProcessBytes() when the blackhole option is disabled.
TEST(OgHttp2AdapterTest, ConnectionErrorWithoutBlackholingData) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.blackhole_data_on_connection_error = false;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence().ClientPreface().WindowUpdate(1, 42).Serialize();

  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kWrongFrameSequence));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_LT(result, 0);

  // Ask the connection to process more bytes. Because the option is disabled,
  // ProcessBytes() should continue to return an error.
  const std::string next_frame = TestFrameSequence().Ping(42).Serialize();
  const int64_t next_result = adapter->ProcessBytes(next_frame);
  EXPECT_LT(next_result, 0);
}

TEST(OgHttp2AdapterTest, ServerDoesNotSendFramesAfterImmediateGoAway) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  // Submit a custom initial SETTINGS frame with one setting.
  adapter->SubmitSettings({{HEADER_TABLE_SIZE, 100u}});

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  // Submit a response for the stream.
  auto body = absl::make_unique<TestDataFrameSource>(visitor, true);
  body->AppendPayload("This data is doomed to never be written.");
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}}), std::move(body));
  ASSERT_EQ(0, submit_result);

  // Submit a WINDOW_UPDATE frame.
  adapter->SubmitWindowUpdate(kConnectionStreamId, 42);

  // Submit another SETTINGS frame.
  adapter->SubmitSettings({});

  // Submit some metadata.
  auto source = absl::make_unique<TestMetadataSource>(ToHeaderBlock(ToHeaders(
      {{"query-cost", "is too darn high"}, {"secret-sauce", "hollandaise"}})));
  adapter->SubmitMetadata(1, 16384u, std::move(source));

  EXPECT_TRUE(adapter->want_write());

  // Trigger a connection error. Only the response headers will be written.
  const std::string connection_error_frames =
      TestFrameSequence().WindowUpdate(3, 42).Serialize();

  EXPECT_CALL(visitor, OnFrameHeader(3, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnConnectionError(ConnectionError::kWrongFrameSequence));

  const int64_t result = adapter->ProcessBytes(connection_error_frames);
  EXPECT_EQ(static_cast<size_t>(result), connection_error_frames.size());

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 6, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 6, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(GOAWAY, 0, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(GOAWAY, 0, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));

  int send_result = adapter->Send();
  // Some bytes should have been serialized.
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::GOAWAY}));
  visitor.Clear();

  // Try to submit more frames for writing. They should not be written.
  adapter->SubmitPing(42);
  // TODO(diannahu): Enable the below expectation.
  // EXPECT_FALSE(adapter->want_write());
  send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(), testing::IsEmpty());
}

TEST(OgHttp2AdapterTest, ServerHandlesContentLength) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::string stream_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1, {{":method", "GET"},
                       {":scheme", "https"},
                       {":authority", "example.com"},
                       {":path", "/this/is/request/one"},
                       {"content-length", "2"}})
          .Data(1, "hi", /*fin=*/true)
          .Headers(3,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/three"},
                    {"content-length", "nan"}},
                   /*fin=*/true)
          .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // Stream 1: content-length is correct
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 2));
  EXPECT_CALL(visitor, OnDataForStream(1, "hi"));
  EXPECT_CALL(visitor, OnEndStream(1));

  // Stream 3: content-length is not a number
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(3, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerHandlesContentLengthMismatch) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::string stream_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1, {{":method", "GET"},
                       {":scheme", "https"},
                       {":authority", "example.com"},
                       {":path", "/this/is/request/two"},
                       {"content-length", "2"}})
          .Data(1, "h", /*fin=*/true)
          .Headers(3, {{":method", "GET"},
                       {":scheme", "https"},
                       {":authority", "example.com"},
                       {":path", "/this/is/request/three"},
                       {"content-length", "2"}})
          .Data(3, "howdy", /*fin=*/true)
          .Headers(5,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/four"},
                    {"content-length", "2"}},
                   /*fin=*/true)
          .Headers(7,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/four"},
                    {"content-length", "2"}},
                   /*fin=*/false)
          .Data(7, "h", /*fin=*/false)
          .Headers(7, {{"extra-info", "Trailers with content-length mismatch"}},
                   /*fin=*/true)
          .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // Stream 1: content-length is larger than actual data
  // All data is delivered to the visitor. Note that neither oghttp2 nor
  // nghttp2 delivers OnInvalidFrame().
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, 1));
  EXPECT_CALL(visitor, OnDataForStream(1, "h"));

  // Stream 3: content-length is smaller than actual data
  // The beginning of data is delivered to the visitor, but not the actual data.
  // Again, neither oghttp2 nor nghttp2 delivers OnInvalidFrame().
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, DATA, 1));
  EXPECT_CALL(visitor, OnBeginDataForStream(3, 5));

  // Stream 5: content-length is invalid and HEADERS ends the stream
  // Only oghttp2 invokes OnEndHeadersForStream(). Only nghttp2 invokes
  // OnInvalidFrame().
  EXPECT_CALL(visitor, OnFrameHeader(5, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(5));
  EXPECT_CALL(visitor, OnHeaderForStream(5, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(5));

  // Stream 7: content-length is invalid and trailers end the stream
  // Only oghttp2 invokes OnEndHeadersForStream(). Only nghttp2 invokes
  // OnInvalidFrame().
  EXPECT_CALL(visitor, OnFrameHeader(7, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(7));
  EXPECT_CALL(visitor, OnHeaderForStream(7, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(7));
  EXPECT_CALL(visitor, OnFrameHeader(7, _, DATA, 0));
  EXPECT_CALL(visitor, OnBeginDataForStream(7, 1));
  EXPECT_CALL(visitor, OnDataForStream(7, "h"));
  EXPECT_CALL(visitor, OnFrameHeader(7, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(7));
  EXPECT_CALL(visitor, OnHeaderForStream(7, _, _));
  EXPECT_CALL(visitor, OnEndHeadersForStream(7));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 5, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 5, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(5, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 7, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 7, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(7, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::RST_STREAM,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerHandlesAsteriskPathForOptions) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::string stream_frames = TestFrameSequence()
                                        .ClientPreface()
                                        .Headers(1,
                                                 {{":scheme", "https"},
                                                  {":authority", "example.com"},
                                                  {":path", "*"},
                                                  {":method", "OPTIONS"}},
                                                 /*fin=*/true)
                                        .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));

  EXPECT_TRUE(adapter->want_write());
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS}));
}

TEST(OgHttp2AdapterTest, ServerHandlesInvalidPath) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::string stream_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "*"},
                    {":method", "GET"}},
                   /*fin=*/true)
          .Headers(3,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "other/non/slash/starter"},
                    {":method", "GET"}},
                   /*fin=*/true)
          .Headers(5,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", ""},
                    {":method", "GET"}},
                   /*fin=*/true)
          .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor,
              OnInvalidFrame(
                  1, Http2VisitorInterface::InvalidFrameError::kHttpMessaging));

  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(visitor,
              OnInvalidFrame(
                  3, Http2VisitorInterface::InvalidFrameError::kHttpMessaging));

  EXPECT_CALL(visitor, OnFrameHeader(5, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(5));
  EXPECT_CALL(visitor, OnHeaderForStream(5, _, _)).Times(2);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(5, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 5, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 5, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(5, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::RST_STREAM,
                    SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerHandlesTeHeader) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::string stream_frames = TestFrameSequence()
                                        .ClientPreface()
                                        .Headers(1,
                                                 {{":scheme", "https"},
                                                  {":authority", "example.com"},
                                                  {":path", "/"},
                                                  {":method", "GET"},
                                                  {"te", "trailers"}},
                                                 /*fin=*/true)
                                        .Headers(3,
                                                 {{":scheme", "https"},
                                                  {":authority", "example.com"},
                                                  {":path", "/"},
                                                  {":method", "GET"},
                                                  {"te", "trailers, deflate"}},
                                                 /*fin=*/true)
                                        .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // Stream 1: TE: trailers should be allowed.
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(5);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  // Stream 3: TE: <non-trailers> should be rejected.
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(3, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerHandlesConnectionSpecificHeaders) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  testing::InSequence s;

  const std::string stream_frames =
      TestFrameSequence()
          .ClientPreface()
          .Headers(1,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/"},
                    {":method", "GET"},
                    {"connection", "keep-alive"}},
                   /*fin=*/true)
          .Headers(3,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/"},
                    {":method", "GET"},
                    {"proxy-connection", "keep-alive"}},
                   /*fin=*/true)
          .Headers(5,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/"},
                    {":method", "GET"},
                    {"keep-alive", "timeout=42"}},
                   /*fin=*/true)
          .Headers(7,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/"},
                    {":method", "GET"},
                    {"transfer-encoding", "chunked"}},
                   /*fin=*/true)
          .Headers(9,
                   {{":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/"},
                    {":method", "GET"},
                    {"upgrade", "h2c"}},
                   /*fin=*/true)
          .Serialize();

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  // All streams contain a connection-specific header and should be rejected.
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(1, Http2VisitorInterface::InvalidFrameError::kHttpHeader));
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(3, Http2VisitorInterface::InvalidFrameError::kHttpHeader));
  EXPECT_CALL(visitor, OnFrameHeader(5, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(5));
  EXPECT_CALL(visitor, OnHeaderForStream(5, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(5, Http2VisitorInterface::InvalidFrameError::kHttpHeader));
  EXPECT_CALL(visitor, OnFrameHeader(7, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(7));
  EXPECT_CALL(visitor, OnHeaderForStream(7, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(7, Http2VisitorInterface::InvalidFrameError::kHttpHeader));
  EXPECT_CALL(visitor, OnFrameHeader(9, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(9));
  EXPECT_CALL(visitor, OnHeaderForStream(9, _, _)).Times(4);
  EXPECT_CALL(
      visitor,
      OnInvalidFrame(9, Http2VisitorInterface::InvalidFrameError::kHttpHeader));

  const int64_t stream_result = adapter->ProcessBytes(stream_frames);
  EXPECT_EQ(stream_frames.size(), static_cast<size_t>(stream_result));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 1, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 1, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(1, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 3, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 3, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(3, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 5, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 5, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(5, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 7, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 7, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(7, Http2ErrorCode::HTTP2_NO_ERROR));
  EXPECT_CALL(visitor, OnBeforeFrameSent(RST_STREAM, 9, _, 0x0));
  EXPECT_CALL(visitor,
              OnFrameSent(RST_STREAM, 9, _, 0x0,
                          static_cast<int>(Http2ErrorCode::PROTOCOL_ERROR)));
  EXPECT_CALL(visitor, OnCloseStream(9, Http2ErrorCode::HTTP2_NO_ERROR));

  EXPECT_TRUE(adapter->want_write());
  int result = adapter->Send();
  EXPECT_EQ(0, result);
  EXPECT_THAT(
      visitor.data(),
      EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::RST_STREAM,
                    SpdyFrameType::RST_STREAM, SpdyFrameType::RST_STREAM,
                    SpdyFrameType::RST_STREAM}));
}

TEST(OgHttp2AdapterTest, ServerUsesCustomWindowUpdateStrategy) {
  // Test the use of a custom WINDOW_UPDATE strategy.
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.should_window_update_fn = [](int64_t /*limit*/, int64_t /*size*/,
                                       int64_t /*delta*/) { return true; };
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/this/is/request/one"}},
                                          /*fin=*/false)
                                 .Data(1, "This is the request body.",
                                       /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnFrameHeader(1, _, DATA, 0x1));
  EXPECT_CALL(visitor, OnBeginDataForStream(1, _));
  EXPECT_CALL(visitor, OnDataForStream(1, "This is the request body."));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<int64_t>(frames.size()), result);

  // Mark a small number of bytes for the stream as consumed. Because of the
  // custom WINDOW_UPDATE strategy, the session should send WINDOW_UPDATEs.
  adapter->MarkDataConsumedForStream(1, 5);

  EXPECT_TRUE(adapter->want_write());

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, 0, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, 0, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(WINDOW_UPDATE, 1, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(WINDOW_UPDATE, 1, 4, 0x0, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(WINDOW_UPDATE, 0, 4, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(WINDOW_UPDATE, 0, 4, 0x0, 0));

  int send_result = adapter->Send();
  EXPECT_EQ(0, send_result);
  EXPECT_THAT(visitor.data(),
              EqualsFrames({SpdyFrameType::SETTINGS, SpdyFrameType::SETTINGS,
                            SpdyFrameType::WINDOW_UPDATE,
                            SpdyFrameType::WINDOW_UPDATE}));
}

// Verifies that NoopHeaderValidator allows several header combinations that
// would otherwise be invalid.
TEST(OgHttp2AdapterTest, NoopHeaderValidatorTest) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  options.validate_http_headers = false;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames = TestFrameSequence()
                                 .ClientPreface()
                                 .Headers(1,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/1"},
                                           {"content-length", "7"},
                                           {"content-length", "7"}},
                                          /*fin=*/false)
                                 .Headers(3,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/3"},
                                           {"content-length", "11"},
                                           {"content-length", "13"}},
                                          /*fin=*/false)
                                 .Headers(5,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "foo.com"},
                                           {":path", "/"},
                                           {"host", "bar.com"}},
                                          /*fin=*/true)
                                 .Headers(7,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/"},
                                           {"Accept", "uppercase, oh boy!"}},
                                          /*fin=*/false)
                                 .Headers(9,
                                          {{":method", "POST"},
                                           {":scheme", "https"},
                                           {":authority", "ex|ample.com"},
                                           {":path", "/"}},
                                          /*fin=*/false)
                                 .Headers(11,
                                          {{":method", "GET"},
                                           {":scheme", "https"},
                                           {":authority", "example.com"},
                                           {":path", "/"},
                                           {"content-length", "nan"}},
                                          /*fin=*/true)
                                 .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 0, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor, OnSettingsEnd());
  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, ":path", "/1"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "content-length", "7"));
  EXPECT_CALL(visitor, OnHeaderForStream(1, "content-length", "7"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  // Stream 3
  EXPECT_CALL(visitor, OnFrameHeader(3, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(3));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, ":path", "/3"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, "content-length", "11"));
  EXPECT_CALL(visitor, OnHeaderForStream(3, "content-length", "13"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(3));
  // Stream 5
  EXPECT_CALL(visitor, OnFrameHeader(5, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(5));
  EXPECT_CALL(visitor, OnHeaderForStream(5, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(5, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(5, ":authority", "foo.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(5, ":path", "/"));
  EXPECT_CALL(visitor, OnHeaderForStream(5, "host", "bar.com"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(5));
  EXPECT_CALL(visitor, OnEndStream(5));
  // Stream 7
  EXPECT_CALL(visitor, OnFrameHeader(7, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(7));
  EXPECT_CALL(visitor, OnHeaderForStream(7, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(7, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(7, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(7, ":path", "/"));
  EXPECT_CALL(visitor, OnHeaderForStream(7, "Accept", "uppercase, oh boy!"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(7));
  // Stream 9
  EXPECT_CALL(visitor, OnFrameHeader(9, _, HEADERS, 4));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(9));
  EXPECT_CALL(visitor, OnHeaderForStream(9, ":method", "POST"));
  EXPECT_CALL(visitor, OnHeaderForStream(9, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(9, ":authority", "ex|ample.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(9, ":path", "/"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(9));
  // Stream 11
  EXPECT_CALL(visitor, OnFrameHeader(11, _, HEADERS, 5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(11));
  EXPECT_CALL(visitor, OnHeaderForStream(11, ":method", "GET"));
  EXPECT_CALL(visitor, OnHeaderForStream(11, ":scheme", "https"));
  EXPECT_CALL(visitor, OnHeaderForStream(11, ":authority", "example.com"));
  EXPECT_CALL(visitor, OnHeaderForStream(11, ":path", "/"));
  EXPECT_CALL(visitor, OnHeaderForStream(11, "content-length", "nan"));
  EXPECT_CALL(visitor, OnEndHeadersForStream(11));
  EXPECT_CALL(visitor, OnEndStream(11));

  const int64_t result = adapter->ProcessBytes(frames);
  EXPECT_EQ(frames.size(), static_cast<size_t>(result));
}

TEST(OgHttp2AdapterTest, NegativeFlowControlStreamResumption) {
  DataSavingVisitor visitor;
  OgHttp2Adapter::Options options;
  options.perspective = Perspective::kServer;
  auto adapter = OgHttp2Adapter::Create(visitor, options);

  const std::string frames =
      TestFrameSequence()
          .ClientPreface({{INITIAL_WINDOW_SIZE, 128u * 1024u}})
          .WindowUpdate(0, 1 << 20)
          .Headers(1,
                   {{":method", "GET"},
                    {":scheme", "https"},
                    {":authority", "example.com"},
                    {":path", "/this/is/request/one"}},
                   /*fin=*/true)
          .Serialize();
  testing::InSequence s;

  // Client preface (empty SETTINGS)
  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor,
              OnSetting(Http2Setting{Http2KnownSettingsId::INITIAL_WINDOW_SIZE,
                                     128u * 1024u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  EXPECT_CALL(visitor, OnFrameHeader(0, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(0, 1 << 20));

  // Stream 1
  EXPECT_CALL(visitor, OnFrameHeader(1, _, HEADERS, 0x5));
  EXPECT_CALL(visitor, OnBeginHeadersForStream(1));
  EXPECT_CALL(visitor, OnHeaderForStream(1, _, _)).Times(4);
  EXPECT_CALL(visitor, OnEndHeadersForStream(1));
  EXPECT_CALL(visitor, OnEndStream(1));

  const int64_t read_result = adapter->ProcessBytes(frames);
  EXPECT_EQ(static_cast<size_t>(read_result), frames.size());

  // Submit a response for the stream.
  auto body = absl::make_unique<TestDataFrameSource>(visitor, true);
  TestDataFrameSource& body_ref = *body;
  body_ref.AppendPayload(std::string(70000, 'a'));
  int submit_result = adapter->SubmitResponse(
      1, ToHeaders({{":status", "200"}}), std::move(body));
  ASSERT_EQ(0, submit_result);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x0));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x0, 0));

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  EXPECT_CALL(visitor, OnBeforeFrameSent(HEADERS, 1, _, 0x4));
  EXPECT_CALL(visitor, OnFrameSent(HEADERS, 1, _, 0x4, 0));
  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, _, 0x0, 0)).Times(5);

  adapter->Send();
  EXPECT_FALSE(adapter->want_write());

  EXPECT_CALL(visitor, OnFrameHeader(0, 6, SETTINGS, 0));
  EXPECT_CALL(visitor, OnSettingsStart());
  EXPECT_CALL(visitor,
              OnSetting(Http2Setting{Http2KnownSettingsId::INITIAL_WINDOW_SIZE,
                                     64u * 1024u}));
  EXPECT_CALL(visitor, OnSettingsEnd());

  // Processing these SETTINGS will cause stream 1's send window to become
  // negative.
  adapter->ProcessBytes(TestFrameSequence()
                            .Settings({{INITIAL_WINDOW_SIZE, 64u * 1024u}})
                            .Serialize());
  EXPECT_TRUE(adapter->want_write());
  EXPECT_LT(adapter->GetStreamSendWindowSize(1), 0);

  body_ref.AppendPayload("Stream should be resumed.");
  adapter->ResumeStream(1);

  EXPECT_CALL(visitor, OnBeforeFrameSent(SETTINGS, 0, _, 0x1));
  EXPECT_CALL(visitor, OnFrameSent(SETTINGS, 0, _, 0x1, 0));
  adapter->Send();
  EXPECT_FALSE(adapter->want_write());

  // Upon receiving the WINDOW_UPDATE, stream 1 should be ready to write.
  EXPECT_CALL(visitor, OnFrameHeader(1, 4, WINDOW_UPDATE, 0));
  EXPECT_CALL(visitor, OnWindowUpdate(1, 10000));
  adapter->ProcessBytes(TestFrameSequence().WindowUpdate(1, 10000).Serialize());
  EXPECT_TRUE(adapter->want_write());
  EXPECT_GT(adapter->GetStreamSendWindowSize(1), 0);

  EXPECT_CALL(visitor, OnFrameSent(DATA, 1, _, 0x0, 0));
  adapter->Send();
}

}  // namespace
}  // namespace test
}  // namespace adapter
}  // namespace http2
