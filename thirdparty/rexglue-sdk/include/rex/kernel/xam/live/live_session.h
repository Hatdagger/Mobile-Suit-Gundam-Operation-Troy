/**
 * Xbox Live matches (sessions) for online / LAN play.
 *
 * Ported from Xenia Canary Netplay (xsession.cc, user_property.cc,
 * XLiveAPI.cpp; BSD licence), simplified for one local player.
 *
 * - Properties and contexts the game sets (XUserSetContext/Property) are
 *   kept in a PropertyStore and uploaded for sessions this PC hosts: the
 *   server only lists sessions in searches once they are set.
 * - XSession is the kernel object behind XamSessionCreateHandle. Messages
 *   from xgi_app (create, join, search, ...) are routed to it.
 * - Only used when network_mode is online or lan; offline keeps the old stubs.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/kernel/xam/live/live_api.h>
#include <rex/kernel/xam/live/live_properties.h>
#include <rex/kernel/xam/live/xnet.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

namespace rex::system {
class KernelState;
}

namespace rex::kernel::xam::live {

//=============================================================================
// Guest structures
//=============================================================================

struct XSESSION_INFO {
  XNKID session_id;
  XNADDR host_address;
  XNKEY key_exchange_key;
};
static_assert(sizeof(XSESSION_INFO) == 0x3C);

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  rex::be<uint32_t> open_public_slots;
  rex::be<uint32_t> open_private_slots;
  rex::be<uint32_t> filled_public_slots;
  rex::be<uint32_t> filled_private_slots;
  rex::be<uint32_t> properties_count;
  rex::be<uint32_t> contexts_count;
  rex::be<uint32_t> properties_ptr;
  rex::be<uint32_t> contexts_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT) == 0x5C);

struct XSESSION_SEARCHRESULT_HEADER {
  rex::be<uint32_t> results_count;
  rex::be<uint32_t> results_ptr;
};

struct XSESSION_MEMBER {
  rex::be<uint64_t> online_xuid;
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> flags;  // 1 = private slot, 2 = zombie
};
static_assert(sizeof(XSESSION_MEMBER) == 0x10);

struct XSESSION_LOCAL_DETAILS {
  rex::be<uint32_t> user_index_host;
  rex::be<uint32_t> game_type;
  rex::be<uint32_t> game_mode;
  rex::be<uint32_t> flags;
  rex::be<uint32_t> max_public_slots;
  rex::be<uint32_t> max_private_slots;
  rex::be<uint32_t> available_public_slots;
  rex::be<uint32_t> available_private_slots;
  rex::be<uint32_t> actual_member_count;
  rex::be<uint32_t> returned_member_count;
  rex::be<uint32_t> state;  // 0 lobby, 1 registration, 2 in game, 3 reporting, 4 deleted
  uint32_t pad;
  rex::be<uint64_t> nonce;
  XSESSION_INFO session_info;
  XNKID arbitration_id;
  rex::be<uint32_t> members_ptr;
};
static_assert(sizeof(XSESSION_LOCAL_DETAILS) == 0x80);

// XUSER_PROPERTY: id + X_USER_DATA (type, pad, 8-byte union).
struct XUSER_PROPERTY {
  rex::be<uint32_t> property_id;
  uint32_t pad0;
  uint8_t type;
  uint8_t pad1[7];
  uint8_t value[8];
};
static_assert(sizeof(XUSER_PROPERTY) == 0x18);

struct XUSER_CONTEXT {
  rex::be<uint32_t> context_id;
  rex::be<uint32_t> value;
};

// Session create flags.
inline constexpr uint32_t kSessionHost = 0x01;
inline constexpr uint32_t kSessionPresence = 0x02;
inline constexpr uint32_t kSessionStats = 0x04;
inline constexpr uint32_t kSessionMatchmaking = 0x08;
inline constexpr uint32_t kSessionArbitration = 0x10;
inline constexpr uint32_t kSessionPeerNetwork = 0x20;
inline constexpr uint32_t kSessionJoinInProgressDisabled = 0x0400;
inline constexpr uint32_t kSessionLiveFeatures =
    kSessionPresence | kSessionStats | kSessionMatchmaking | kSessionArbitration;

// Result codes the session messages return.
inline constexpr uint32_t kXOnlineSessionWrongState = 0x80155206;
inline constexpr uint32_t kXOnlineSessionFull = 0x80155207;
inline constexpr uint32_t kXOnlineSessionNotLoggedOn = 0x80155209;

//=============================================================================
// The session object
//=============================================================================

class XSession : public rex::system::XObject {
 public:
  static const Type kObjectType = Type::Session;

  explicit XSession(rex::system::KernelState* kernel_state);
  ~XSession() override;

  uint32_t Initialize();

  uint32_t Create(uint32_t user_index, uint32_t public_slots, uint32_t private_slots,
                  uint32_t flags, uint32_t session_info_ptr, uint32_t nonce_ptr);
  uint32_t Delete();
  uint32_t Join(uint32_t count, uint32_t xuids_ptr, uint32_t indices_ptr, uint32_t privates_ptr);
  uint32_t Leave(uint32_t count, uint32_t xuids_ptr, uint32_t indices_ptr);
  uint32_t Start();
  uint32_t End();
  uint32_t Modify(uint32_t flags, uint32_t public_slots, uint32_t private_slots);
  uint32_t GetDetails(uint32_t buffer_size, uint32_t details_ptr);
  uint32_t MigrateHost(uint32_t session_info_ptr, uint32_t user_index);
  uint32_t RegisterArbitration(uint32_t results_size, uint32_t results_ptr);

  bool created() const { return created_; }
  bool is_host() const { return host_; }
  bool is_live() const { return (flags_ & kSessionLiveFeatures) != 0; }
  uint64_t session_id() const { return session_id_; }

  // Uploads properties for hosted live sessions if they changed. Cheap when unchanged.
  void SyncProperties();
  // Calls SyncProperties on every hosted session (after the game sets a property).
  static void SyncAllProperties(rex::system::KernelState* kernel_state);

  // Searches: write an XSESSION_SEARCHRESULT_HEADER + results into the game's buffer.
  static uint32_t Search(rex::system::KernelState* kernel_state, uint32_t query_index,
                         uint32_t user_index, uint32_t max_results, uint32_t num_props,
                         uint32_t props_ptr, uint32_t num_ctx, uint32_t ctx_ptr,
                         uint32_t results_size, uint32_t results_ptr, uint32_t num_users);
  static uint32_t SearchByIds(rex::system::KernelState* kernel_state,
                              const std::vector<uint64_t>& ids, uint32_t results_size,
                              uint32_t results_ptr);

 private:
  void WriteSessionInfo(XSESSION_INFO* info) const;

  bool created_ = false;
  bool host_ = false;
  bool deleted_ = false;
  uint32_t state_ = 0;
  uint32_t flags_ = 0;
  uint32_t user_index_ = 0;
  uint32_t max_public_ = 0, max_private_ = 0;
  uint32_t open_public_ = 0, open_private_ = 0;
  uint64_t session_id_ = 0;
  uint64_t nonce_ = 0;
  XSESSION_INFO info_{};
  std::map<uint64_t, XSESSION_MEMBER> members_;
  uint64_t uploaded_revision_ = UINT64_MAX;
  uint32_t members_guest_ptr_ = 0;
};

// Both: true when sessions are real (network_mode online or lan).
bool SessionsEnabled();

}  // namespace rex::kernel::xam::live
