#ifndef SRC_QUIT_WEB_TRANSPORT_VISITORS_H_
#define SRC_QUIT_WEB_TRANSPORT_VISITORS_H_

#include <string>

#include "absl/status/status.h"
#include "quiche/common/platform/api/quiche_logging.h"
#include "quiche/common/platform/api/quiche_mem_slice.h"
#include "quiche/common/quiche_circular_deque.h"
#include "quiche/common/quiche_stream.h"
#include "quiche/common/simple_buffer_allocator.h"
#include "quiche/quic/core/web_transport_interface.h"
#include "quiche/quic/platform/api/quic_logging.h"
#include "quiche/spdy/core/http2_header_block.h"

using namespace quic;

namespace quit {

// Discards any incoming data.
class WebTransportDiscardVisitor : public WebTransportStreamVisitor {
 public:
  WebTransportDiscardVisitor(WebTransportStream* stream) : stream_(stream) {}

  void OnCanRead() override {
    std::string buffer;
    WebTransportStream::ReadResult result = stream_->Read(&buffer);
    QUIC_DVLOG(2) << "Read " << result.bytes_read
                  << " bytes from WebTransport stream "
                  << stream_->GetStreamId() << ", fin: " << result.fin;
  }

  void OnCanWrite() override {}

  void OnResetStreamReceived(WebTransportStreamError /*error*/) override {}
  void OnStopSendingReceived(WebTransportStreamError /*error*/) override {}
  void OnWriteSideInDataRecvdState() override {}

 private:
  WebTransportStream* stream_;
};

// Echoes any incoming data back on the same stream.
class WebTransportBidirectionalEchoVisitor : public WebTransportStreamVisitor {
 public:
  WebTransportBidirectionalEchoVisitor(WebTransportStream* stream)
      : stream_(stream) {}

  void OnCanRead() override {
    WebTransportStream::ReadResult result = stream_->Read(&buffer_);
    QUIC_DVLOG(1) << "Attempted reading on WebTransport bidirectional stream "
                  << stream_->GetStreamId()
                  << ", bytes read: " << result.bytes_read
                  << ", data: " << buffer_;
    if (result.fin) {
      send_fin_ = true;
    }
    OnCanWrite();
  }

  void OnCanWrite() override {
    if (stop_sending_received_) {
      return;
    }

    if (!buffer_.empty()) {
      absl::Status status = quiche::WriteIntoStream(*stream_, buffer_);
      QUIC_DVLOG(1) << "Attempted writing on WebTransport bidirectional stream "
                    << stream_->GetStreamId() << ", success: " << status;
      if (!status.ok()) {
        return;
      }

      buffer_ = "";
    }

    if (send_fin_ && !fin_sent_) {
      absl::Status status = quiche::SendFinOnStream(*stream_);
      if (status.ok()) {
        fin_sent_ = true;
      }
    }
  }

  void OnResetStreamReceived(WebTransportStreamError /*error*/) override {
    // Send FIN in response to a stream reset.  We want to test that we can
    // operate one side of the stream cleanly while the other is reset, thus
    // replying with a FIN rather than a RESET_STREAM is more appropriate here.
    send_fin_ = true;
    OnCanWrite();
  }
  void OnStopSendingReceived(WebTransportStreamError /*error*/) override {
    stop_sending_received_ = true;
  }
  void OnWriteSideInDataRecvdState() override {}

 protected:
  WebTransportStream* stream() { return stream_; }

 private:
  WebTransportStream* stream_;
  std::string buffer_;
  bool send_fin_ = false;
  bool fin_sent_ = false;
  bool stop_sending_received_ = false;
};

// Buffers all of the data and calls |callback| with the entirety of the stream
// data.
class WebTransportUnidirectionalEchoReadVisitor
    : public WebTransportStreamVisitor {
 public:
  using Callback = std::function<void(const std::string&)>;

  WebTransportUnidirectionalEchoReadVisitor(WebTransportStream* stream,
                                            Callback callback)
      : stream_(stream), callback_(std::move(callback)) {}

  void OnCanRead() override {
    WebTransportStream::ReadResult result = stream_->Read(&buffer_);
    QUIC_DVLOG(1) << "Attempted reading on WebTransport unidirectional stream "
                  << stream_->GetStreamId()
                  << ", bytes read: " << result.bytes_read;
    if (result.fin) {
      QUIC_DVLOG(1) << "Finished receiving data on a WebTransport stream "
                    << stream_->GetStreamId() << ", queueing up the echo";
      callback_(buffer_);
    }
  }

  void OnCanWrite() override { QUICHE_NOTREACHED(); }

  void OnResetStreamReceived(WebTransportStreamError /*error*/) override {}
  void OnStopSendingReceived(WebTransportStreamError /*error*/) override {}
  void OnWriteSideInDataRecvdState() override {}

 private:
  WebTransportStream* stream_;
  std::string buffer_;
  Callback callback_;
};

// Sends supplied data.
class WebTransportUnidirectionalEchoWriteVisitor
    : public WebTransportStreamVisitor {
 public:
  WebTransportUnidirectionalEchoWriteVisitor(WebTransportStream* stream,
                                             const std::string& data)
      : stream_(stream), data_(data) {}

  void OnCanRead() override { QUICHE_NOTREACHED(); }
  void OnCanWrite() override {
    if (data_.empty()) {
      return;
    }
    absl::Status write_status = quiche::WriteIntoStream(*stream_, data_);
    if (!write_status.ok()) {
      QUICHE_DLOG_IF(WARNING, !absl::IsUnavailable(write_status))
          << "Failed to write into stream: " << write_status;
      return;
    }
    data_ = "";
    absl::Status fin_status = quiche::SendFinOnStream(*stream_);
    QUICHE_DVLOG(1)
        << "WebTransportUnidirectionalEchoWriteVisitor finished sending data.";
    QUICHE_DCHECK(fin_status.ok());
  }

  void OnResetStreamReceived(WebTransportStreamError /*error*/) override {}
  void OnStopSendingReceived(WebTransportStreamError /*error*/) override {}
  void OnWriteSideInDataRecvdState() override {}

 private:
  WebTransportStream* stream_;
  std::string data_;
};

// A session visitor which sets unidirectional or bidirectional stream visitors
// to echo.
class EchoWebTransportSessionVisitor : public WebTransportVisitor {
 public:
  EchoWebTransportSessionVisitor(WebTransportSession* session)
      : session_(session) {}

  void OnSessionReady() override {
    QUIC_LOG(INFO) << "OnSessionReady";
    if (session_->CanOpenNextOutgoingBidirectionalStream()) {
      OnCanCreateNewOutgoingBidirectionalStream();
    }
  }

  void OnSessionClosed(WebTransportSessionError /*error_code*/,
                       const std::string& /*error_message*/) override {
    QUIC_LOG(INFO) << "OnSessionClosed";
  }

  void OnIncomingBidirectionalStreamAvailable() override {
    QUIC_LOG(INFO) << "OnIncomingBidirectionalStreamAvailable";
    while (true) {
      WebTransportStream* stream =
          session_->AcceptIncomingBidirectionalStream();
      if (stream == nullptr) {
        return;
      }
      QUIC_DVLOG(1)
          << "EchoWebTransportSessionVisitor received a bidirectional stream "
          << stream->GetStreamId();
      stream->SetVisitor(
          std::make_unique<WebTransportBidirectionalEchoVisitor>(stream));
      stream->visitor()->OnCanRead();
    }
  }

  void OnIncomingUnidirectionalStreamAvailable() override {
    QUIC_LOG(INFO) << "OnIncomingUnidirectionalStreamAvailable";
    while (true) {
      WebTransportStream* stream =
          session_->AcceptIncomingUnidirectionalStream();
      if (stream == nullptr) {
        return;
      }
      QUIC_DVLOG(1)
          << "EchoWebTransportSessionVisitor received a unidirectional stream";
      stream->SetVisitor(
          std::make_unique<WebTransportUnidirectionalEchoReadVisitor>(
              stream, [this](const std::string& data) {
                streams_to_echo_back_.push_back(data);
                TrySendingUnidirectionalStreams();
              }));
      stream->visitor()->OnCanRead();
    }
  }

  void OnDatagramReceived(absl::string_view datagram) override {
    QUIC_LOG(INFO) << "OnDatagramReceived";
    std::string resp("who am I");
    session_->SendOrQueueDatagram(resp);
  }

  void OnCanCreateNewOutgoingBidirectionalStream() override {
    if (!echo_stream_opened_) {
      WebTransportStream* stream = session_->OpenOutgoingBidirectionalStream();
      stream->SetVisitor(
          std::make_unique<WebTransportBidirectionalEchoVisitor>(stream));
      echo_stream_opened_ = true;
    }
  }
  void OnCanCreateNewOutgoingUnidirectionalStream() override {
    TrySendingUnidirectionalStreams();
  }

  void TrySendingUnidirectionalStreams() {
    while (!streams_to_echo_back_.empty() &&
           session_->CanOpenNextOutgoingUnidirectionalStream()) {
      QUIC_DVLOG(1)
          << "EchoWebTransportServer echoed a unidirectional stream back";
      WebTransportStream* stream = session_->OpenOutgoingUnidirectionalStream();
      stream->SetVisitor(
          std::make_unique<WebTransportUnidirectionalEchoWriteVisitor>(
              stream, streams_to_echo_back_.front()));
      streams_to_echo_back_.pop_front();
      stream->visitor()->OnCanWrite();
    }
  }

 private:
  WebTransportSession* session_;
  quiche::SimpleBufferAllocator allocator_;
  bool echo_stream_opened_ = false;

  quiche::QuicheCircularDeque<std::string> streams_to_echo_back_;
};

}  // namespace quit

#endif /* SRC_QUIT_WEB_TRANSPORT_VISITORS_H_ */
