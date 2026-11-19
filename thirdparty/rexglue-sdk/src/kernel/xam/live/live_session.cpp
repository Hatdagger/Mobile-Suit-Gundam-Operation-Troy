/**
 * Xbox Live matches (sessions). See live_session.h.
 *
 * Ported from Xenia Canary Netplay (BSD licence), simplified.
 */

#include <rex/kernel/xam/live/live_session.h>

#include <algorithm>
#include <cstring>
#include <set>

#include <rex/kernel/xam/live/live_identity.h>
#include <rex/kernel/xam/live/live_service.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xmemory.h>

namespace rex::kernel::xam::live {

using rex::system::KernelState;

namespace {

constexpr uint32_t X_ERROR_SUCCESS_ = 0;
constexpr uint32_t X_E_INVALIDARG_ = 0x80070057;
constexpr uint32_t X_E_FAIL_ = 0x80004005;
constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER_ = 0x7A;

std::mutex g_sessions_mutex;
std::set<XSession*> g_sessions;

uint64_t OwnOnlineXuid(KernelState* ks) {
  if (ks && ks->user_profile()) {
    return ks->user_profile()->online_xuid();
  }
  return 0;
}

void FillXnAddrFromSession(const SessionInfo& s, XNADDR* addr) {
  std::memset(addr, 0, sizeof(XNADDR));
  const uint32_t ip = ParseIpv4(s.host_address);
  addr->ina.addr = ip;
  addr->inaOnline.addr = ip;
  addr->wPortOnline = s.port ? s.port : kPlayerPort;
  uint64_t mac = s.mac_address;
  for (int i = 5; i >= 0; --i) {
    addr->abEnet[i] = static_cast<uint8_t>(mac & 0xFF);
    mac >>= 8;
  }
  addr->abOnline[kXnAddrPlatformTypeOffset] = kPlatformXbox360;
}

void FillSearchResult(const SessionInfo& s, XSESSION_SEARCHRESULT* r) {
  std::memset(r, 0, sizeof(*r));
  r->info.session_id.set_u64(s.session_id);
  FillXnAddrFromSession(s, &r->info.host_address);
  GenerateIdentityExchangeKey(&r->info.key_exchange_key);
  r->open_public_slots = s.open_public_slots;
  r->open_private_slots = s.open_private_slots;
  r->filled_public_slots = s.filled_public_slots;
  r->filled_private_slots = s.filled_private_slots;
}

std::shared_ptr<LiveApi> Api() {
  return LiveService::Get().api();
}

}  // namespace

bool SessionsEnabled() {
  return LiveService::Get().mode() != NetworkMode::kOffline;
}

//=============================================================================
// XSession
//=============================================================================

XSession::XSession(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSession::~XSession() {
  std::lock_guard lock(g_sessions_mutex);
  g_sessions.erase(this);
}

uint32_t XSession::Initialize() {
  // X_KSESSION: just the handle.
  auto native = CreateNative(sizeof(uint32_t));
  if (!native) {
    return 0xC0000017;  // X_STATUS_NO_MEMORY
  }
  *reinterpret_cast<rex::be<uint32_t>*>(native) = handle();
  return 0;
}

void XSession::WriteSessionInfo(XSESSION_INFO* info) const {
  *info = info_;
}

uint32_t XSession::Create(uint32_t user_index, uint32_t public_slots, uint32_t private_slots,
                          uint32_t flags, uint32_t session_info_ptr, uint32_t nonce_ptr) {
  if (created_) {
    return kXOnlineSessionWrongState;
  }
  if (!session_info_ptr) {
    return X_E_INVALIDARG_;
  }
  auto& live = LiveService::Get();
  flags_ = flags;
  user_index_ = user_index;
  if (is_live() && !live.SignedInToLive() && live.mode() == NetworkMode::kOnline) {
    return kXOnlineSessionNotLoggedOn;
  }
  auto* info = memory()->TranslateVirtual<XSESSION_INFO*>(session_info_ptr);
  const uint64_t xuid = OwnOnlineXuid(kernel_state());
  PropertyStore::Get().AddDefaults(xuid, live.gamertag());

  const bool hosting = (flags & kSessionHost) || !is_live();
  if (hosting) {
    host_ = true;
    if (nonce_ptr) {
      nonce_ = GenerateSessionId(0) ^ (uint64_t(handle()) << 32);
      *memory()->TranslateVirtual<rex::be<uint64_t>*>(nonce_ptr) = nonce_;
    }
    session_id_ = GenerateSessionId(is_live() ? XNKID_ONLINE_PEER : XNKID_SYSTEM_LINK);
    std::memset(&info_, 0, sizeof(info_));
    info_.session_id.set_u64(session_id_);
    FillOwnXnAddr(&info_.host_address);
    GenerateIdentityExchangeKey(&info_.key_exchange_key);
    if (is_live()) {
      if (auto api = Api()) {
        SessionInfo s;
        s.session_id = session_id_;
        s.host_xuid = xuid;
        s.title = "Gundam Operation Troy";
        s.version = "1.0";
        s.flags = flags;
        s.public_slots = public_slots;
        s.private_slots = private_slots;
        s.user_index = user_index;
        s.host_address = Ipv4ToString(live.online_ip() ? live.online_ip() : live.local_ip());
        s.mac_address = live.identity().mac_address;
        s.port = live.player_port();
        if (!api->CreateSession(s)) {
          REXKRNL_ERROR("Live: creating session {:016X} failed: {}", session_id_,
                        api->last_result().message);
        } else {
          REXKRNL_INFO("Live: created session {:016X} (flags {:08X}, {} public, {} private)",
                       session_id_, flags, public_slots, private_slots);
        }
      }
    }
  } else {
    // Joining someone else's session: the game filled in its ID (from a search).
    info_ = *info;
    session_id_ = info_.session_id.as_u64();
    if (auto api = Api(); api && IsOnlinePeerSession(session_id_)) {
      if (auto s = api->GetSession(session_id_); s && !s->host_address.empty()) {
        FillXnAddrFromSession(*s, &info_.host_address);
      }
    }
    REXKRNL_INFO("Live: joining session {:016X}", session_id_);
  }
  WriteSessionInfo(info);
  max_public_ = open_public_ = public_slots;
  max_private_ = open_private_ = private_slots;
  state_ = 0;
  created_ = true;
  {
    std::lock_guard lock(g_sessions_mutex);
    g_sessions.insert(this);
  }
  SyncProperties();
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::Delete() {
  if (!created_) {
    return X_E_INVALIDARG_;
  }
  if (deleted_) {
    return kXOnlineSessionWrongState;
  }
  deleted_ = true;
  state_ = 4;
  if (host_ && is_live()) {
    if (auto api = Api()) {
      api->DeleteSession(session_id_);
      REXKRNL_INFO("Live: deleted session {:016X}", session_id_);
    }
  }
  std::lock_guard lock(g_sessions_mutex);
  g_sessions.erase(this);
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::Join(uint32_t count, uint32_t xuids_ptr, uint32_t indices_ptr,
                        uint32_t privates_ptr) {
  if (!count || (xuids_ptr != 0) == (indices_ptr != 0)) {
    return X_E_INVALIDARG_;
  }
  if (deleted_) {
    return kXOnlineSessionWrongState;
  }
  if (state_ == 2 && (flags_ & kSessionJoinInProgressDisabled)) {
    return kXOnlineSessionFull;
  }
  auto* xuids = xuids_ptr ? memory()->TranslateVirtual<rex::be<uint64_t>*>(xuids_ptr) : nullptr;
  auto* privates =
      privates_ptr ? memory()->TranslateVirtual<rex::be<uint32_t>*>(privates_ptr) : nullptr;
  const uint64_t own_xuid = OwnOnlineXuid(kernel_state());
  std::vector<std::pair<uint64_t, bool>> added;
  for (uint32_t i = 0; i < count; ++i) {
    XSESSION_MEMBER member{};
    uint64_t xuid;
    if (xuids) {
      xuid = xuids[i];
      member.user_index = (xuid == own_xuid) ? 0 : 0xFE;
    } else {
      const uint32_t index = memory()->TranslateVirtual<rex::be<uint32_t>*>(indices_ptr)[i];
      if (index != 0) {
        return X_E_INVALIDARG_;  // one local player
      }
      xuid = own_xuid;
      member.user_index = index;
    }
    if (members_.count(xuid)) {
      continue;
    }
    member.online_xuid = xuid;
    const bool want_private = privates && privates[i];
    if (want_private && open_private_ > 0) {
      member.flags = 1;
      --open_private_;
    } else if (open_public_ > 0) {
      --open_public_;
    }
    members_[xuid] = member;
    added.emplace_back(xuid, member.flags & 1);
  }
  if (!added.empty() && is_live()) {
    if (auto api = Api()) {
      if (host_) {
        api->JoinSession(session_id_, added);
      } else {
        std::vector<uint64_t> ids;
        for (auto& [x, p] : added) {
          ids.push_back(x);
        }
        api->PreJoinSession(session_id_, ids);
      }
    }
  }
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::Leave(uint32_t count, uint32_t xuids_ptr, uint32_t indices_ptr) {
  if (!count || (xuids_ptr != 0) == (indices_ptr != 0)) {
    return X_E_INVALIDARG_;
  }
  const uint64_t own_xuid = OwnOnlineXuid(kernel_state());
  std::vector<uint64_t> removed;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t xuid = own_xuid;
    if (xuids_ptr) {
      xuid = memory()->TranslateVirtual<rex::be<uint64_t>*>(xuids_ptr)[i];
    }
    auto it = members_.find(xuid);
    if (it == members_.end()) {
      continue;
    }
    if (it->second.flags & 1) {
      open_private_ = std::min(open_private_ + 1, max_private_);
    } else {
      open_public_ = std::min(open_public_ + 1, max_public_);
    }
    members_.erase(it);
    removed.push_back(xuid);
  }
  if (!removed.empty() && is_live() && host_) {
    if (auto api = Api()) {
      api->LeaveSession(session_id_, removed);
    }
  }
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::Start() {
  if (!created_ || deleted_) {
    return kXOnlineSessionWrongState;
  }
  state_ = 2;
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::End() {
  if (!created_ || deleted_) {
    return kXOnlineSessionWrongState;
  }
  state_ = 0;
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::Modify(uint32_t flags, uint32_t public_slots, uint32_t private_slots) {
  if (!created_ || deleted_) {
    return kXOnlineSessionWrongState;
  }
  // Only the join-related modifier bits may change.
  const uint32_t kModifiers = 0x0F80;
  flags_ = (flags_ & ~kModifiers) | (flags & kModifiers);
  const uint32_t used_public = max_public_ - open_public_;
  const uint32_t used_private = max_private_ - open_private_;
  max_public_ = public_slots;
  max_private_ = private_slots;
  open_public_ = public_slots > used_public ? public_slots - used_public : 0;
  open_private_ = private_slots > used_private ? private_slots - used_private : 0;
  if (host_ && is_live()) {
    if (auto api = Api()) {
      api->ModifySession(session_id_, flags_, public_slots, private_slots);
    }
  }
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::GetDetails(uint32_t buffer_size, uint32_t details_ptr) {
  if (!details_ptr || buffer_size < sizeof(XSESSION_LOCAL_DETAILS)) {
    return X_ERROR_INSUFFICIENT_BUFFER_;
  }
  // On a joined live session, refresh members from the server.
  if (!host_ && is_live()) {
    if (auto api = Api()) {
      if (auto s = api->GetSessionDetails(session_id_)) {
        for (uint64_t xuid : s->player_xuids) {
          if (!members_.count(xuid)) {
            XSESSION_MEMBER m{};
            m.online_xuid = xuid;
            m.user_index = 0xFE;
            members_[xuid] = m;
          }
        }
      }
    }
  }
  auto* d = memory()->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(details_ptr);
  std::memset(d, 0, sizeof(*d));
  d->user_index_host = host_ ? user_index_ : 0xFE;
  d->game_type = PropertyStore::Get().GetContext(kContextGameType).value_or(0);
  d->game_mode = PropertyStore::Get().GetContext(kContextGameMode).value_or(0);
  d->flags = flags_;
  d->max_public_slots = max_public_;
  d->max_private_slots = max_private_;
  d->available_public_slots = open_public_;
  d->available_private_slots = open_private_;
  d->actual_member_count = uint32_t(members_.size());
  d->state = state_;
  d->nonce = nonce_;
  d->session_info = info_;
  // Members go after the details if the buffer has room, else in system memory.
  const uint32_t count = uint32_t(members_.size());
  uint32_t members_ptr = 0;
  const uint32_t needed = sizeof(XSESSION_LOCAL_DETAILS) + count * sizeof(XSESSION_MEMBER);
  if (count && buffer_size >= needed) {
    members_ptr = details_ptr + sizeof(XSESSION_LOCAL_DETAILS);
  } else if (count) {
    if (!members_guest_ptr_) {
      members_guest_ptr_ = memory()->SystemHeapAlloc(64 * sizeof(XSESSION_MEMBER));
    }
    members_ptr = members_guest_ptr_;
  }
  uint32_t written = 0;
  if (members_ptr) {
    auto* m = memory()->TranslateVirtual<XSESSION_MEMBER*>(members_ptr);
    for (const auto& [xuid, member] : members_) {
      if (written >= 64) {
        break;
      }
      m[written++] = member;
    }
  }
  d->returned_member_count = written;
  d->members_ptr = members_ptr;
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::MigrateHost(uint32_t session_info_ptr, uint32_t user_index) {
  if (!created_ || deleted_ || !session_info_ptr) {
    return X_E_INVALIDARG_;
  }
  auto* info = memory()->TranslateVirtual<XSESSION_INFO*>(session_info_ptr);
  if (is_live()) {
    auto api = Api();
    if (!api) {
      return X_E_FAIL_;
    }
    auto& live = LiveService::Get();
    const std::string ip = Ipv4ToString(live.online_ip() ? live.online_ip() : live.local_ip());
    auto s = api->MigrateSession(session_id_, OwnOnlineXuid(kernel_state()), ip,
                                 live.identity().mac_address, live.player_port());
    if (!s) {
      REXKRNL_ERROR("Live: migrating session {:016X} failed", session_id_);
      return X_E_FAIL_;
    }
    session_id_ = s->session_id;
  }
  host_ = true;
  user_index_ = user_index;
  info_.session_id.set_u64(session_id_);
  FillOwnXnAddr(&info_.host_address);
  *info = info_;
  uploaded_revision_ = UINT64_MAX;
  SyncProperties();
  REXKRNL_INFO("Live: now hosting migrated session {:016X}", session_id_);
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::RegisterArbitration(uint32_t results_size, uint32_t results_ptr) {
  if (!results_ptr || results_size < 8) {
    return X_ERROR_INSUFFICIENT_BUFFER_;
  }
  auto* mem = memory();
  auto* header = mem->TranslateVirtual<rex::be<uint32_t>*>(results_ptr);
  // XSESSION_REGISTRATION_RESULTS {count, ptr} then XSESSION_REGISTRANT[] then XUIDs.
  struct Registrant {
    uint64_t machine_id;
    std::vector<uint64_t> xuids;
  };
  std::vector<Registrant> registrants;
  if (is_live()) {
    if (auto api = Api()) {
      if (auto arb = api->GetArbitration(session_id_)) {
        for (const auto& m : arb->machines) {
          registrants.push_back({m.machine_id, m.xuids});
        }
      }
    }
  }
  if (registrants.empty()) {
    // Fall back to this machine and the members we know.
    Registrant self{LiveService::Get().machine_id(), {}};
    for (const auto& [xuid, m] : members_) {
      self.xuids.push_back(xuid);
    }
    registrants.push_back(self);
  }
  uint32_t total_xuids = 0;
  for (auto& r : registrants) {
    total_xuids += uint32_t(r.xuids.size());
  }
  const uint32_t needed = 8 + uint32_t(registrants.size()) * 0x18 + total_xuids * 8;
  if (results_size < needed) {
    return X_ERROR_INSUFFICIENT_BUFFER_;
  }
  header[0] = uint32_t(registrants.size());
  header[1] = results_ptr + 8;
  uint32_t reg_ptr = results_ptr + 8;
  uint32_t xuid_ptr = reg_ptr + uint32_t(registrants.size()) * 0x18;
  for (const auto& r : registrants) {
    auto* p = mem->TranslateVirtual<uint8_t*>(reg_ptr);
    *reinterpret_cast<rex::be<uint64_t>*>(p) = r.machine_id;
    *reinterpret_cast<rex::be<uint32_t>*>(p + 8) = 10;  // trustworthiness
    *reinterpret_cast<rex::be<uint32_t>*>(p + 12) = uint32_t(r.xuids.size());
    *reinterpret_cast<rex::be<uint32_t>*>(p + 16) = xuid_ptr;
    for (uint64_t x : r.xuids) {
      *mem->TranslateVirtual<rex::be<uint64_t>*>(xuid_ptr) = x;
      xuid_ptr += 8;
    }
    reg_ptr += 0x18;
  }
  state_ = 1;
  return X_ERROR_SUCCESS_;
}

void XSession::SyncProperties() {
  if (!created_ || deleted_ || !host_ || !is_live()) {
    return;
  }
  auto& store = PropertyStore::Get();
  const uint64_t revision = store.revision();
  if (revision == uploaded_revision_) {
    return;
  }
  auto api = Api();
  if (!api) {
    return;
  }
  std::vector<std::string> encoded;
  for (const auto& p : store.All()) {
    encoded.push_back(p.ToBase64());
  }
  if (api->SetSessionProperties(session_id_, encoded)) {
    uploaded_revision_ = revision;
    REXKRNL_INFO("Live: uploaded {} properties for session {:016X}", encoded.size(), session_id_);
  } else {
    REXKRNL_WARN("Live: uploading properties for session {:016X} failed", session_id_);
  }
}

void XSession::SyncAllProperties(KernelState*) {
  std::vector<XSession*> sessions;
  {
    std::lock_guard lock(g_sessions_mutex);
    sessions.assign(g_sessions.begin(), g_sessions.end());
  }
  for (auto* s : sessions) {
    s->SyncProperties();
  }
}

uint32_t XSession::SearchByIds(KernelState* ks, const std::vector<uint64_t>& ids,
                               uint32_t results_size, uint32_t results_ptr) {
  std::vector<SessionInfo> found;
  if (auto api = Api()) {
    for (uint64_t id : ids) {
      if (auto s = api->GetSession(id); s && !s->host_address.empty()) {
        found.push_back(*s);
      }
    }
  }
  auto* mem = ks->memory();
  if (!results_ptr || results_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    return X_ERROR_INSUFFICIENT_BUFFER_;
  }
  std::memset(mem->TranslateVirtual<uint8_t*>(results_ptr), 0, results_size);
  const uint32_t room =
      (results_size - sizeof(XSESSION_SEARCHRESULT_HEADER)) / sizeof(XSESSION_SEARCHRESULT);
  const uint32_t count = std::min<uint32_t>(room, uint32_t(found.size()));
  auto* header = mem->TranslateVirtual<XSESSION_SEARCHRESULT_HEADER*>(results_ptr);
  const uint32_t first = results_ptr + sizeof(XSESSION_SEARCHRESULT_HEADER);
  auto* results = mem->TranslateVirtual<XSESSION_SEARCHRESULT*>(first);
  for (uint32_t i = 0; i < count; ++i) {
    FillSearchResult(found[i], &results[i]);
  }
  header->results_count = count;
  header->results_ptr = first;
  return X_ERROR_SUCCESS_;
}

uint32_t XSession::Search(KernelState* ks, uint32_t query_index, uint32_t user_index,
                          uint32_t max_results, uint32_t num_props, uint32_t props_ptr,
                          uint32_t num_ctx, uint32_t ctx_ptr, uint32_t results_size,
                          uint32_t results_ptr, uint32_t num_users) {
  auto& live = LiveService::Get();
  if (!live.Connected()) {
    return kXOnlineSessionNotLoggedOn;
  }
  auto api = live.api();
  if (!api || !results_ptr || results_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    return X_ERROR_INSUFFICIENT_BUFFER_;
  }
  auto* mem = ks->memory();
  SessionSearchQuery query;
  query.search_index = query_index;
  query.results_count = max_results;
  query.num_users = num_users ? num_users : 1;
  query.searcher_xuid = OwnOnlineXuid(ks);
  if (ctx_ptr) {
    auto* ctx = mem->TranslateVirtual<XUSER_CONTEXT*>(ctx_ptr);
    for (uint32_t i = 0; i < num_ctx; ++i) {
      // Some games pass INT32 properties in the contexts array (Xenia note).
      const uint32_t id = ctx[i].context_id;
      const uint8_t raw[4] = {uint8_t(uint32_t(ctx[i].value) >> 24),
                              uint8_t(uint32_t(ctx[i].value) >> 16),
                              uint8_t(uint32_t(ctx[i].value) >> 8), uint8_t(uint32_t(ctx[i].value))};
      query.filters.push_back(Property::FromGuest(id, raw).ToBase64());
    }
  }
  if (props_ptr) {
    auto* props = mem->TranslateVirtual<XUSER_PROPERTY*>(props_ptr);
    for (uint32_t i = 0; i < num_props; ++i) {
      Property p;
      p.id = props[i].property_id;
      p.type = PropertyType(p.id);
      std::memcpy(p.value, props[i].value, 8);
      if (p.has_extra()) {
        // Filters on strings/blobs are rare; send the header only.
        p.extra.clear();
      }
      query.filters.push_back(p.ToBase64());
    }
  }
  auto sessions = api->SearchSessions(query);
  if (!sessions) {
    REXKRNL_WARN("Live: session search failed: {}", api->last_result().message);
    sessions.emplace();
  }
  REXKRNL_INFO("Live: session search (query {}, {} filters) found {}", query_index,
               query.filters.size(), sessions->size());

  // Layout: header, results[], then contexts/properties (and string data).
  std::memset(mem->TranslateVirtual<uint8_t*>(results_ptr), 0, results_size);
  const uint32_t first = results_ptr + sizeof(XSESSION_SEARCHRESULT_HEADER);
  const uint32_t room =
      (results_size - sizeof(XSESSION_SEARCHRESULT_HEADER)) / sizeof(XSESSION_SEARCHRESULT);
  const uint32_t count =
      std::min<uint32_t>({room, max_results ? max_results : room, uint32_t(sessions->size())});
  uint32_t cursor = first + count * sizeof(XSESSION_SEARCHRESULT);
  const uint32_t end = results_ptr + results_size;
  auto alloc = [&](uint32_t size) -> uint32_t {
    size = (size + 7) & ~7u;
    if (!size) {
      return 0;
    }
    if (cursor + size <= end) {
      uint32_t p = cursor;
      cursor += size;
      return p;
    }
    return mem->SystemHeapAlloc(size);
  };
  auto* results = mem->TranslateVirtual<XSESSION_SEARCHRESULT*>(first);
  for (uint32_t i = 0; i < count; ++i) {
    const SessionInfo& s = (*sessions)[i];
    FillSearchResult(s, &results[i]);
    std::vector<Property> contexts, properties;
    if (auto list = api->GetSessionProperties(s.session_id, query_index)) {
      for (const auto& text : *list) {
        if (auto p = Property::FromBase64(text)) {
          (p->is_context() ? contexts : properties).push_back(*p);
        }
      }
    }
    if (!contexts.empty()) {
      const uint32_t ptr = alloc(uint32_t(contexts.size() * sizeof(XUSER_CONTEXT)));
      if (ptr) {
        auto* c = mem->TranslateVirtual<XUSER_CONTEXT*>(ptr);
        for (size_t k = 0; k < contexts.size(); ++k) {
          c[k].context_id = contexts[k].id;
          c[k].value = contexts[k].u32();
        }
        results[i].contexts_count = uint32_t(contexts.size());
        results[i].contexts_ptr = ptr;
      }
    }
    if (!properties.empty()) {
      const uint32_t ptr = alloc(uint32_t(properties.size() * sizeof(XUSER_PROPERTY)));
      if (ptr) {
        auto* out = mem->TranslateVirtual<XUSER_PROPERTY*>(ptr);
        for (size_t k = 0; k < properties.size(); ++k) {
          const Property& p = properties[k];
          std::memset(&out[k], 0, sizeof(XUSER_PROPERTY));
          out[k].property_id = p.id;
          out[k].type = static_cast<uint8_t>(p.type);
          std::memcpy(out[k].value, p.value, 8);
          if (p.has_extra()) {
            const uint32_t data = alloc(uint32_t(p.extra.size()));
            if (data) {
              std::memcpy(mem->TranslateVirtual<uint8_t*>(data), p.extra.data(), p.extra.size());
            }
            const uint32_t size = data ? uint32_t(p.extra.size()) : 0;
            const uint8_t v[8] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8),
                                  uint8_t(size),       uint8_t(data >> 24), uint8_t(data >> 16),
                                  uint8_t(data >> 8),  uint8_t(data)};
            std::memcpy(out[k].value, v, 8);
          }
        }
        results[i].properties_count = uint32_t(properties.size());
        results[i].properties_ptr = ptr;
      }
    }
  }
  auto* header = mem->TranslateVirtual<XSESSION_SEARCHRESULT_HEADER*>(results_ptr);
  header->results_count = count;
  header->results_ptr = first;
  (void)user_index;
  return X_ERROR_SUCCESS_;
}

}  // namespace rex::kernel::xam::live
