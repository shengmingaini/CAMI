# CAMI 统一信封 — Cap'n Proto 统一协议 (阶段 A)
# 转写自 proto/flatbuffers/envelope.fbs。可靠轨 (轨道 B) 消息容器。
# body 为 union, 自动生成判别枚举 Body::Which。
# 高频轨道 A (位压缩 UDP) 不携此结构。
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("cami");
@0xcA11c0de00000009;

using Login = import "login.capnp";
using Movement = import "movement.capnp";
using Aoi = import "aoi.capnp";
using Combat = import "combat.capnp";
using Quest = import "quest.capnp";
using Economy = import "economy.capnp";
using Social = import "social.capnp";

struct MessageEnvelope {
  protoVersion @0 :UInt16;   # 协议版本 (v1=1)
  seq @1 :UInt32;            # 每连接每方向单调递增序号 (防重放)
  requestId @2 :UInt32;      # 请求/响应关联 (0 = 单向广播)
  flags @3 :UInt8;           # bit0=加密, bit1=压缩, bit2=需响应
  union {
    clientHello @4 :Login.ClientHello;
    serverHello @5 :Login.ServerHello;
    heartbeat @6 :Login.Heartbeat;
    disconnectNotify @7 :Login.DisconnectNotify;
    migrateNotify @8 :Login.MigrateNotify;
    loginRequest @9 :Login.LoginRequest;
    loginResponse @10 :Login.LoginResponse;
    enterWorldRequest @11 :Login.EnterWorldRequest;
    enterWorldResponse @12 :Login.EnterWorldResponse;
    movementSnapshot @13 :Movement.MovementSnapshot;
    correction @14 :Movement.Correction;
    aoiEnter @15 :Aoi.AoiEnter;
    aoiLeave @16 :Aoi.AoiLeave;
    aoiUpdate @17 :Aoi.AoiUpdate;
    skillCastIntent @18 :Combat.SkillCastIntent;
    skillCastResult @19 :Combat.SkillCastResult;
    damageEvent @20 :Combat.DamageEvent;
    buffEvent @21 :Combat.BuffEvent;
    combatResult @22 :Combat.CombatResult;
    questAcceptIntent @23 :Quest.QuestAcceptIntent;
    questTurnInIntent @24 :Quest.QuestTurnInIntent;
    questProgress @25 :Quest.QuestProgress;
    questRewardResult @26 :Quest.QuestRewardResult;
    questLogSnapshot @27 :Quest.QuestLogSnapshot;
    tradeIntent @28 :Economy.TradeIntent;
    shopBuy @29 :Economy.ShopBuy;
    shopSell @30 :Economy.ShopSell;
    auctionList @31 :Economy.AuctionList;
    auctionBid @32 :Economy.AuctionBid;
    mailSend @33 :Economy.MailSend;
    tradeResult @34 :Economy.TradeResult;
    shopResult @35 :Economy.ShopResult;
    auctionUpdate @36 :Economy.AuctionUpdate;
    mailList @37 :Economy.MailList;
    currencyGranted @38 :Economy.CurrencyGranted;
    friendAdd @39 :Social.FriendAdd;
    guildCreate @40 :Social.GuildCreate;
    partyInvite @41 :Social.PartyInvite;
    chatSend @42 :Social.ChatSend;
    friendList @43 :Social.FriendList;
    guildInfo @44 :Social.GuildInfo;
    partyUpdate @45 :Social.PartyUpdate;
    chatBroadcast @46 :Social.ChatBroadcast;
    achievementUpdate @47 :Social.AchievementUpdate;
    reputationUpdate @48 :Social.ReputationUpdate;
  }
}
