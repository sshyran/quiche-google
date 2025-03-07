#include "quiche/spdy/core/metadata_extension.h"

#include "absl/container/flat_hash_map.h"
#include "absl/functional/bind_front.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "quiche/common/platform/api/quiche_test.h"
#include "quiche/spdy/core/array_output_buffer.h"
#include "quiche/spdy/core/spdy_framer.h"
#include "quiche/spdy/core/spdy_header_block.h"
#include "quiche/spdy/core/spdy_no_op_visitor.h"
#include "quiche/spdy/core/spdy_protocol.h"
#include "quiche/spdy/test_tools/mock_spdy_framer_visitor.h"

namespace spdy {
namespace test {
namespace {

using ::absl::bind_front;
using ::spdy::SpdyFramer;
using ::spdy::SpdyHeaderBlock;
using ::spdy::test::MockSpdyFramerVisitor;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

const size_t kBufferSize = 64 * 1024;
char kBuffer[kBufferSize];

class MetadataExtensionTest : public quiche::test::QuicheTest {
 protected:
  MetadataExtensionTest() : test_buffer_(kBuffer, kBufferSize) {}

  void SetUp() override {
    extension_ = absl::make_unique<MetadataVisitor>(
        bind_front(&MetadataExtensionTest::OnCompletePayload, this),
        bind_front(&MetadataExtensionTest::OnMetadataSupport, this));
  }

  void OnCompletePayload(spdy::SpdyStreamId stream_id,
                         MetadataVisitor::MetadataPayload payload) {
    ++received_count_;
    received_payload_map_.insert(std::make_pair(stream_id, std::move(payload)));
  }

  void OnMetadataSupport(bool peer_supports_metadata) {
    EXPECT_EQ(peer_supports_metadata, extension_->PeerSupportsMetadata());
    received_metadata_support_.push_back(peer_supports_metadata);
  }

  MetadataSerializer::MetadataPayload PayloadForData(absl::string_view data) {
    SpdyHeaderBlock block;
    block["example-payload"] = data;
    return block;
  }

  std::unique_ptr<MetadataVisitor> extension_;
  absl::flat_hash_map<spdy::SpdyStreamId, SpdyHeaderBlock>
      received_payload_map_;
  std::vector<bool> received_metadata_support_;
  size_t received_count_ = 0;
  spdy::ArrayOutputBuffer test_buffer_;
};

// This test verifies that the MetadataVisitor is initialized to a state where
// it believes the peer does not support metadata.
TEST_F(MetadataExtensionTest, MetadataNotSupported) {
  EXPECT_FALSE(extension_->PeerSupportsMetadata());
  EXPECT_THAT(received_metadata_support_, IsEmpty());
}

// This test verifies that upon receiving a specific setting, the extension
// realizes that the peer supports metadata.
TEST_F(MetadataExtensionTest, MetadataSupported) {
  EXPECT_FALSE(extension_->PeerSupportsMetadata());
  // 3 is not an appropriate value for the metadata extension key.
  extension_->OnSetting(MetadataVisitor::kMetadataExtensionId, 3);
  EXPECT_FALSE(extension_->PeerSupportsMetadata());
  extension_->OnSetting(MetadataVisitor::kMetadataExtensionId, 1);
  ASSERT_TRUE(extension_->PeerSupportsMetadata());
  extension_->OnSetting(MetadataVisitor::kMetadataExtensionId, 0);
  EXPECT_FALSE(extension_->PeerSupportsMetadata());
  EXPECT_THAT(received_metadata_support_, ElementsAre(true, false));
}

TEST_F(MetadataExtensionTest, MetadataIgnoredWithoutExtension) {
  const char kData[] = "some payload";
  SpdyHeaderBlock payload = PayloadForData(kData);

  extension_->OnSetting(MetadataVisitor::kMetadataExtensionId, 1);
  ASSERT_TRUE(extension_->PeerSupportsMetadata());

  MetadataSerializer serializer;
  auto sequence = serializer.FrameSequenceForPayload(3, std::move(payload));
  ASSERT_TRUE(sequence != nullptr);

  http2::Http2DecoderAdapter deframer;
  ::testing::StrictMock<MockSpdyFramerVisitor> visitor;
  deframer.set_visitor(&visitor);

  EXPECT_CALL(visitor,
              OnCommonHeader(3, _, MetadataVisitor::kMetadataFrameType, _));
  // The Return(true) should not be necessary. http://b/36023792
  EXPECT_CALL(visitor, OnUnknownFrame(3, MetadataVisitor::kMetadataFrameType))
      .WillOnce(::testing::Return(true));

  SpdyFramer framer(SpdyFramer::ENABLE_COMPRESSION);
  auto frame = sequence->Next();
  ASSERT_TRUE(frame != nullptr);
  while (frame != nullptr) {
    const size_t frame_size = framer.SerializeFrame(*frame, &test_buffer_);
    ASSERT_GT(frame_size, 0u);
    ASSERT_FALSE(deframer.HasError());
    ASSERT_EQ(frame_size, test_buffer_.Size());
    EXPECT_EQ(frame_size, deframer.ProcessInput(kBuffer, frame_size));
    test_buffer_.Reset();
    frame = sequence->Next();
  }
  EXPECT_FALSE(deframer.HasError());
  EXPECT_THAT(received_metadata_support_, ElementsAre(true));
}

// This test verifies that the METADATA frame emitted by a MetadataExtension
// can be parsed by another SpdyFramer with a MetadataVisitor.
TEST_F(MetadataExtensionTest, MetadataPayloadEndToEnd) {
  SpdyHeaderBlock block1;
  block1["foo"] = "Some metadata value.";
  SpdyHeaderBlock block2;
  block2["bar"] =
      "The color taupe truly represents a triumph of the human spirit over "
      "adversity.";
  block2["baz"] =
      "Or perhaps it represents abject surrender to the implacable and "
      "incomprehensible forces of the universe.";
  const absl::string_view binary_payload{"binary\0payload", 14};
  block2["qux"] = binary_payload;
  EXPECT_EQ(binary_payload, block2["qux"]);
  for (const SpdyHeaderBlock& payload_block :
       {std::move(block1), std::move(block2)}) {
    extension_->OnSetting(MetadataVisitor::kMetadataExtensionId, 1);
    ASSERT_TRUE(extension_->PeerSupportsMetadata());

    MetadataSerializer serializer;
    auto sequence =
        serializer.FrameSequenceForPayload(3, payload_block.Clone());
    ASSERT_TRUE(sequence != nullptr);

    http2::Http2DecoderAdapter deframer;
    ::spdy::SpdyNoOpVisitor visitor;
    deframer.set_visitor(&visitor);
    deframer.set_extension_visitor(extension_.get());
    SpdyFramer framer(SpdyFramer::ENABLE_COMPRESSION);
    auto frame = sequence->Next();
    ASSERT_TRUE(frame != nullptr);
    while (frame != nullptr) {
      const size_t frame_size = framer.SerializeFrame(*frame, &test_buffer_);
      ASSERT_GT(frame_size, 0u);
      ASSERT_FALSE(deframer.HasError());
      ASSERT_EQ(frame_size, test_buffer_.Size());
      EXPECT_EQ(frame_size, deframer.ProcessInput(kBuffer, frame_size));
      test_buffer_.Reset();
      frame = sequence->Next();
    }
    EXPECT_EQ(1u, received_count_);
    auto it = received_payload_map_.find(3);
    ASSERT_TRUE(it != received_payload_map_.end());
    EXPECT_EQ(payload_block, it->second);

    received_count_ = 0;
    received_payload_map_.clear();
  }
}

// This test verifies that METADATA frames for two different streams can be
// interleaved and still successfully parsed by another SpdyFramer with a
// MetadataVisitor.
TEST_F(MetadataExtensionTest, MetadataPayloadInterleaved) {
  const std::string kData1 = std::string(65 * 1024, 'a');
  const std::string kData2 = std::string(65 * 1024, 'b');
  const SpdyHeaderBlock payload1 = PayloadForData(kData1);
  const SpdyHeaderBlock payload2 = PayloadForData(kData2);

  extension_->OnSetting(MetadataVisitor::kMetadataExtensionId, 1);
  ASSERT_TRUE(extension_->PeerSupportsMetadata());

  MetadataSerializer serializer;
  auto sequence1 = serializer.FrameSequenceForPayload(3, payload1.Clone());
  ASSERT_TRUE(sequence1 != nullptr);

  auto sequence2 = serializer.FrameSequenceForPayload(5, payload2.Clone());
  ASSERT_TRUE(sequence2 != nullptr);

  http2::Http2DecoderAdapter deframer;
  ::spdy::SpdyNoOpVisitor visitor;
  deframer.set_visitor(&visitor);
  deframer.set_extension_visitor(extension_.get());

  SpdyFramer framer(SpdyFramer::ENABLE_COMPRESSION);
  auto frame1 = sequence1->Next();
  ASSERT_TRUE(frame1 != nullptr);
  auto frame2 = sequence2->Next();
  ASSERT_TRUE(frame2 != nullptr);
  while (frame1 != nullptr || frame2 != nullptr) {
    for (auto frame : {frame1.get(), frame2.get()}) {
      if (frame != nullptr) {
        const size_t frame_size = framer.SerializeFrame(*frame, &test_buffer_);
        ASSERT_GT(frame_size, 0u);
        ASSERT_FALSE(deframer.HasError());
        ASSERT_EQ(frame_size, test_buffer_.Size());
        EXPECT_EQ(frame_size, deframer.ProcessInput(kBuffer, frame_size));
        test_buffer_.Reset();
      }
    }
    frame1 = sequence1->Next();
    frame2 = sequence2->Next();
  }
  EXPECT_EQ(2u, received_count_);
  auto it = received_payload_map_.find(3);
  ASSERT_TRUE(it != received_payload_map_.end());
  EXPECT_EQ(payload1, it->second);

  it = received_payload_map_.find(5);
  ASSERT_TRUE(it != received_payload_map_.end());
  EXPECT_EQ(payload2, it->second);
}

}  // anonymous namespace
}  // namespace test
}  // namespace spdy
