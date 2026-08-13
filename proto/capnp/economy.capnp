# CAMI 经济/交易/商店/拍卖/邮件 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/economy.fbs。轨道 B 可靠消息。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000007;

using Common = import "common.capnp";

# 币种 (与 config 侧 CurrencyType 值保持一致)
enum CurrencyType :UInt8 {
  gold;        # 游戏金币 (主货币)
  token;       # 付费代币
  prestige;    # 声望点 (旧占位, 已由 Reputation 接管语义)
  honor;       # 荣誉点 (PvP)
  conquest;    # 征服点 (PvP 赛季)
  justice;     # 正义点 (旧副本)
  valor;       # 勇气点 (旧副本)
  arenaPoints; # 竞技场点
}

enum EconomyResult :UInt8 {
  ok;
  inventoryFull;
  notEnoughCurrency;
  invalidItem;
  outOfRange;
  auctionNotFound;
  bidTooLow;
  mailboxFull;
  tradeCancelled;
}

enum AuctionStatus :UInt8 {
  listed;
  bid;
  sold;
  expired;
  cancelled;
}

enum TradeAction :UInt8 {
  invite;
  offer;       # 更新报价
  confirm;     # 锁定确认
  cancel;
}

# ---- C->S 意图 ----
struct TradeIntent {
  fromId @0 :UInt64;
  toId @1 :UInt64;
  action @2 :TradeAction;
  items @3 :List(Common.ItemGrant);
  currency @4 :UInt64;
}

struct ShopBuy {
  playerId @0 :UInt64;
  npcKey @1 :Text;
  itemId @2 :UInt32;
  count @3 :UInt32;
}

struct ShopSell {
  playerId @0 :UInt64;
  npcKey @1 :Text;
  itemId @2 :UInt32;
  count @3 :UInt32;
}

struct AuctionList {
  sellerId @0 :UInt64;
  itemId @1 :UInt32;
  count @2 :UInt32;
  bid @3 :UInt64;        # 起拍价
  buyout @4 :UInt64;     # 一口价 (0 = 无)
  durationH @5 :UInt32;  # 挂单时长 (小时)
}

struct AuctionBid {
  bidderId @0 :UInt64;
  listingId @1 :UInt64;
  bid @2 :UInt64;
}

struct MailSend {
  senderId @0 :UInt64;
  receiverId @1 :UInt64;
  subject @2 :Text;
  body @3 :Text;
  items @4 :List(Common.ItemGrant);
  currency @5 :UInt64;
}

# ---- S->C 结果/增量 ----
struct TradeResult {
  requestId @0 :UInt32;
  result @1 :EconomyResult;
  peerId @2 :UInt64;
}

struct ShopResult {
  requestId @0 :UInt32;
  result @1 :EconomyResult;
  itemId @2 :UInt32;
  count @3 :UInt32;
}

struct AuctionUpdate {
  listingId @0 :UInt64;
  status @1 :AuctionStatus;
  topBid @2 :UInt64;
  topBidder @3 :UInt64;
}

struct MailList {
  receiverId @0 :UInt64;
  mailIds @1 :List(UInt64);
  unread @2 :UInt32;
}

struct CurrencyGranted {
  playerId @0 :UInt64;
  currencyType @1 :CurrencyType;
  delta @2 :Int64;      # 正负增减
  newTotal @3 :UInt64;
}
