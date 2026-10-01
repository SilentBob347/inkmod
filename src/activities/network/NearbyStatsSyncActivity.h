#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/reader/GlobalReadingStats.h"

class NearbyStatsSyncActivity final : public Activity {
 public:
  enum class State { STARTING, READY, DISCOVERING, SYNCING, SYNCED, ERROR };

  explicit NearbyStatsSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~NearbyStatsSyncActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state_ == State::DISCOVERING || state_ == State::SYNCING; }

  void enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data, int length);

 private:
  enum class PacketType : uint8_t {
    HELLO = 1,
    STATS = 2,
    ACK = 3,
    NAME = 4,
    BOOK_INFO = 5,
    BOOK_NONE = 6,
    BOOK_ACK = 7,
    FILE_REQUEST = 8,
    FILE_CHUNK = 9,
    FILE_ACK = 10,
    INVALID_STATS = 0xFF
  };

  static constexpr size_t MAX_PACKET_PAYLOAD = 236;

  struct SyncEvent {
    PacketType type = PacketType::HELLO;
    std::array<uint8_t, 6> sourceMac = {};
    std::array<uint8_t, 6> deviceMac = {};
    std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> stats = {};
    std::array<char, 21> deviceName = {};
    std::array<uint8_t, MAX_PACKET_PAYLOAD> payload = {};
    uint8_t statsSize = 0;
    uint8_t payloadSize = 0;
  };
  static constexpr size_t MAX_SYNC_EVENTS = 8;

  State state_ = State::STARTING;
  SemaphoreHandle_t eventMutex_ = nullptr;
  std::array<SyncEvent, MAX_SYNC_EVENTS> events_ = {};
  uint8_t eventHead_ = 0;
  uint8_t eventCount_ = 0;
  bool eventOverflow_ = false;
  bool espNowStarted_ = false;
  bool localStatsReady_ = false;
  bool peerSeen_ = false;
  bool peerStatsSaved_ = false;
  bool localStatsSent_ = false;
  bool localStatsAcked_ = false;

  std::array<uint8_t, 6> localDeviceMac_ = {};
  std::array<uint8_t, 6> peerSourceMac_ = {};
  std::array<uint8_t, 6> peerDeviceMac_ = {};
  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> localStats_ = {};
  uint8_t localStatsSize_ = 0;

  // Current-book nearby sync. We exchange the active book's identity and its
  // progress; if the peer does not have that exact file, it requests the book
  // and receives it in reliable ESP-NOW chunks before the progress is applied.
  std::string localBookPath_;
  std::string localBookName_;
  std::array<uint8_t, 6> localBookProgress_ = {};
  uint8_t localBookProgressSize_ = 0;
  uint64_t localBookSize_ = 0;
  uint32_t localBookCrc32_ = 0;
  bool localBookReady_ = false;
  bool localBookInfoSent_ = false;
  bool localBookAcked_ = false;

  std::string peerBookName_;
  std::string peerBookPath_;
  std::array<uint8_t, 6> peerBookProgress_ = {};
  uint8_t peerBookProgressSize_ = 0;
  uint64_t peerBookSize_ = 0;
  uint32_t peerBookCrc32_ = 0;
  bool peerBookStateSeen_ = false;
  bool peerBookHandled_ = false;

  bool sendingBookFile_ = false;
  uint64_t sendBookOffset_ = 0;
  uint64_t sendBookAwaitingAckOffset_ = 0;
  uint32_t lastBookChunkSendMs_ = 0;

  bool receivingBookFile_ = false;
  uint64_t receiveBookOffset_ = 0;
  std::string receiveBookTempPath_;
  FsFile receiveBookFile_;

  std::string bookStatus_;

  uint32_t syncStartedMs_ = 0;
  uint32_t lastHelloMs_ = 0;
  uint32_t lastStatsSendMs_ = 0;
  std::string peerId_;
  std::string peerName_;
  std::string errorMessage_;

  bool beginEspNow();
  void endEspNow();
  bool prepareLocalStats();
  bool prepareLocalBook();
  void resetBookSyncState();
  bool sendBookState();
  bool sendBookAck(const uint8_t* peerMac);
  bool sendFileRequest(const uint8_t* peerMac);
  bool sendFileAck(const uint8_t* peerMac, uint64_t nextOffset);
  bool sendNextBookChunk(bool retry = false);
  bool handlePeerBookInfo(const SyncEvent& event);
  bool handleBookChunk(const SyncEvent& event);
  bool applyPeerBookProgress(const std::string& localPath);
  bool startReceivingPeerBook();
  bool finalizeReceivedPeerBook();
  void startSync();
  void processEvents();
  void handleEvent(const SyncEvent& event);
  bool sendPacket(PacketType type, const uint8_t* peerMac, const uint8_t* payload = nullptr,
                  uint8_t payloadSize = 0);
  bool sendHello();
  bool sendDeviceName(const uint8_t* peerMac);
  bool sendLocalStats();
  bool sendAck(const uint8_t* peerMac);
  bool addPeer(const uint8_t* peerMac);
  void updateSyncProgress();
  void setState(State state);
  void setError(const std::string& error);
  void renderReady(const std::string& primary, const std::string& detailPrimary,
                   const std::string& detailSecondary) const;
};
