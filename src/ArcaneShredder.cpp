#include "ArcaneShredder.h"

#include "Bag.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "ConfigValueCache.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 DisenchantSpellId = 13262;
constexpr uint32 PickLockSpellId = 1804;
constexpr uint32 ProtocolVersion = 2;
constexpr uint32 AllowedQualityMask = (1u << ITEM_QUALITY_UNCOMMON) | (1u << ITEM_QUALITY_RARE) | (1u << ITEM_QUALITY_EPIC);
constexpr uint32 AllowedBindingMask = 0x03;
constexpr uint32 AllowedBagMask = 0x1F;

enum class ConfigKey : uint8
{
    Enable,
    MaxBatchSize,
    PreviewLifetimeSeconds,
    CommandCooldownMilliseconds,
    AllowInCombat,
    AllowRare,
    AllowEpic,
    AllowRefundable,
    AllowBopTradeable,
    AllowEnchanted,
    AllowSocketed,
    MaxItemLevel,
    ProtectedItemIds,
    AuditLog,
    Count
};

class ArcaneShredderConfig final : public ConfigValueCache<ConfigKey>
{
public:
    static ArcaneShredderConfig& Instance()
    {
        static ArcaneShredderConfig instance;
        return instance;
    }

    void Load(bool reload)
    {
        Initialize(reload);
        Clamp(ConfigKey::MaxBatchSize, 1, 100, "ArcaneShredder.MaxBatchSize");
        Clamp(ConfigKey::PreviewLifetimeSeconds, 5, 300, "ArcaneShredder.PreviewLifetimeSeconds");
        Clamp(ConfigKey::CommandCooldownMilliseconds, 0, 5000, "ArcaneShredder.CommandCooldownMilliseconds");
        Clamp(ConfigKey::MaxItemLevel, 0, 284, "ArcaneShredder.MaxItemLevel");
        ParseProtectedItemIds(GetConfigValue(ConfigKey::ProtectedItemIds));

        LOG_INFO("module",
            "ArcaneShredder: configuration loaded (enabled={}, "
            "maxBatchSize={}, protectedItems={})",
            IsEnabled(), MaxBatchSize(), _protectedItemIds.size());
    }

    bool IsEnabled() const
    {
        return GetConfigValue<bool>(ConfigKey::Enable);
    }

    uint32 MaxBatchSize() const
    {
        return GetConfigValue<uint32>(ConfigKey::MaxBatchSize);
    }

    uint32 PreviewLifetimeSeconds() const
    {
        return GetConfigValue<uint32>(ConfigKey::PreviewLifetimeSeconds);
    }

    uint32 CommandCooldownMilliseconds() const
    {
        return GetConfigValue<uint32>(ConfigKey::CommandCooldownMilliseconds);
    }

    bool AllowInCombat() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowInCombat);
    }

    bool AllowRare() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowRare);
    }

    bool AllowEpic() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowEpic);
    }

    bool AllowRefundable() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowRefundable);
    }

    bool AllowBopTradeable() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowBopTradeable);
    }

    bool AllowEnchanted() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowEnchanted);
    }

    bool AllowSocketed() const
    {
        return GetConfigValue<bool>(ConfigKey::AllowSocketed);
    }

    uint32 MaxItemLevel() const
    {
        return GetConfigValue<uint32>(ConfigKey::MaxItemLevel);
    }

    bool AuditLog() const
    {
        return GetConfigValue<bool>(ConfigKey::AuditLog);
    }

    bool IsProtected(uint32 itemEntry) const
    {
        return _protectedItemIds.find(itemEntry) != _protectedItemIds.end();
    }

protected:
    void BuildConfigCache() override
    {
        SetConfigValue<bool>(ConfigKey::Enable, "ArcaneShredder.Enable", true);
        SetConfigValue<uint32>(ConfigKey::MaxBatchSize, "ArcaneShredder.MaxBatchSize", 25);
        SetConfigValue<uint32>(ConfigKey::PreviewLifetimeSeconds, "ArcaneShredder.PreviewLifetimeSeconds", 30);
        SetConfigValue<uint32>(ConfigKey::CommandCooldownMilliseconds, "ArcaneShredder.CommandCooldownMilliseconds", 500);
        SetConfigValue<bool>(ConfigKey::AllowInCombat, "ArcaneShredder.AllowInCombat", false);
        SetConfigValue<bool>(ConfigKey::AllowRare, "ArcaneShredder.AllowRare", true);
        SetConfigValue<bool>(ConfigKey::AllowEpic, "ArcaneShredder.AllowEpic", false);
        SetConfigValue<bool>(ConfigKey::AllowRefundable, "ArcaneShredder.AllowRefundable", false);
        SetConfigValue<bool>(ConfigKey::AllowBopTradeable, "ArcaneShredder.AllowBopTradeable", false);
        SetConfigValue<bool>(ConfigKey::AllowEnchanted, "ArcaneShredder.AllowEnchanted", false);
        SetConfigValue<bool>(ConfigKey::AllowSocketed, "ArcaneShredder.AllowSocketed", false);
        SetConfigValue<uint32>(ConfigKey::MaxItemLevel, "ArcaneShredder.MaxItemLevel", 0);
        SetConfigValue<std::string>(ConfigKey::ProtectedItemIds, "ArcaneShredder.ProtectedItemIds", "");
        SetConfigValue<bool>(ConfigKey::AuditLog, "ArcaneShredder.AuditLog", true);
    }

private:
    ArcaneShredderConfig()
        : ConfigValueCache(ConfigKey::Count)
    {
    }

    void Clamp(ConfigKey key, uint32 minimum, uint32 maximum, char const* name)
    {
        uint32 value = GetConfigValue<uint32>(key);
        uint32 clamped = std::clamp(value, minimum, maximum);
        if (value != clamped)
        {
            LOG_WARN("module", "ArcaneShredder: {}={} is outside {}..{}; using {}", name, value, minimum, maximum, clamped);
            OverwriteConfigValue<uint32>(key, clamped);
        }
    }

    static std::string_view Trim(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        return value;
    }

    void ParseProtectedItemIds(std::string_view csv)
    {
        _protectedItemIds.clear();

        std::size_t start = 0;
        while (start <= csv.size())
        {
            std::size_t end = csv.find(',', start);
            std::string_view token = Trim(csv.substr(start, end == std::string_view::npos ? csv.size() - start : end - start));
            if (!token.empty())
            {
                uint32 itemEntry = 0;
                auto [position, error] = std::from_chars(token.data(), token.data() + token.size(), itemEntry);
                if (error == std::errc() && position == token.data() + token.size() && itemEntry != 0)
                    _protectedItemIds.insert(itemEntry);
                else
                    LOG_WARN("module", "ArcaneShredder: ignoring invalid ProtectedItemIds entry '{}'", std::string(token));
            }

            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
    }

    std::unordered_set<uint32> _protectedItemIds;
};

enum class EligibilityReason
{
    Eligible,
    PlayerUnavailable,
    PlayerDead,
    InCombat,
    SpellMissing,
    SkillMissing,
    ItemMissing,
    NotOwned,
    NotInInventory,
    Equipped,
    InTrade,
    LootInProgress,
    Refundable,
    BopTradeable,
    Enchanted,
    Socketed,
    TemplateMissing,
    QualityBlocked,
    ClassBlocked,
    SkillTooLow,
    NotLockbox,
    AlreadyOpen,
    NoLootTemplate,
    Protected,
    ItemLevelBlocked,
    BindingBlocked,
    BagBlocked,
    StackUnsupported,
    ItemChanged,
    DeliveryFailed
};

char const* ReasonCode(EligibilityReason reason)
{
    switch (reason)
    {
    case EligibilityReason::Eligible:
        return "ELIGIBLE";
    case EligibilityReason::PlayerUnavailable:
        return "PLAYER_UNAVAILABLE";
    case EligibilityReason::PlayerDead:
        return "PLAYER_DEAD";
    case EligibilityReason::InCombat:
        return "IN_COMBAT";
    case EligibilityReason::SpellMissing:
        return "SPELL_MISSING";
    case EligibilityReason::SkillMissing:
        return "SKILL_MISSING";
    case EligibilityReason::ItemMissing:
        return "ITEM_MISSING";
    case EligibilityReason::NotOwned:
        return "NOT_OWNED";
    case EligibilityReason::NotInInventory:
        return "NOT_IN_INVENTORY";
    case EligibilityReason::Equipped:
        return "EQUIPPED";
    case EligibilityReason::InTrade:
        return "IN_TRADE";
    case EligibilityReason::LootInProgress:
        return "LOOT_IN_PROGRESS";
    case EligibilityReason::Refundable:
        return "REFUNDABLE";
    case EligibilityReason::BopTradeable:
        return "BOP_TRADEABLE";
    case EligibilityReason::Enchanted:
        return "ENCHANTED";
    case EligibilityReason::Socketed:
        return "SOCKETED";
    case EligibilityReason::TemplateMissing:
        return "TEMPLATE_MISSING";
    case EligibilityReason::QualityBlocked:
        return "QUALITY_BLOCKED";
    case EligibilityReason::ClassBlocked:
        return "CLASS_BLOCKED";
    case EligibilityReason::SkillTooLow:
        return "SKILL_TOO_LOW";
    case EligibilityReason::NotLockbox:
        return "NOT_LOCKBOX";
    case EligibilityReason::AlreadyOpen:
        return "ALREADY_OPEN";
    case EligibilityReason::NoLootTemplate:
        return "NO_LOOT_TEMPLATE";
    case EligibilityReason::Protected:
        return "PROTECTED";
    case EligibilityReason::ItemLevelBlocked:
        return "ITEM_LEVEL_BLOCKED";
    case EligibilityReason::BindingBlocked:
        return "BINDING_BLOCKED";
    case EligibilityReason::BagBlocked:
        return "BAG_BLOCKED";
    case EligibilityReason::StackUnsupported:
        return "STACK_UNSUPPORTED";
    case EligibilityReason::ItemChanged:
        return "ITEM_CHANGED";
    case EligibilityReason::DeliveryFailed:
        return "DELIVERY_FAILED";
    }

    return "UNKNOWN";
}

struct PreviewFilter
{
    uint32 qualityMask = 0;
    uint32 maxItemLevel = 0;
    uint32 bindingMask = 0;
    uint32 bagMask = 0;
};

enum class PreviewMode
{
    Disenchant,
    Open
};

struct CandidateSnapshot
{
    ObjectGuid itemGuid;
    uint32 entry = 0;
    uint32 count = 0;
    uint32 quality = 0;
    uint32 itemLevel = 0;
    uint32 flags = 0;
    uint8 bag = 0;
    uint8 slot = 0;
};

struct PreviewSession
{
    ObjectGuid::LowType ownerGuid = 0;
    std::string token;
    PreviewFilter filter;
    PreviewMode mode = PreviewMode::Disenchant;
    std::chrono::steady_clock::time_point expiresAt;
    std::vector<CandidateSnapshot> candidates;
};

struct ExecutionSummary
{
    uint32 completed = 0;
    uint32 skipped = 0;
    uint32 mailed = 0;
};

bool IsScannableInventoryPosition(uint8 bag, uint8 slot)
{
    if (bag == INVENTORY_SLOT_BAG_0)
        return slot >= INVENTORY_SLOT_ITEM_START && slot < INVENTORY_SLOT_ITEM_END;

    return bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END;
}

bool IsBagAllowed(PreviewFilter const& filter, uint8 bag)
{
    uint32 bit = 0;
    if (bag == INVENTORY_SLOT_BAG_0)
        bit = 1u;
    else if (bag >= INVENTORY_SLOT_BAG_START && bag < INVENTORY_SLOT_BAG_END)
        bit = 1u << (bag - INVENTORY_SLOT_BAG_START + 1);

    return bit != 0 && (filter.bagMask & bit) != 0;
}

bool HasRegularEnchantment(Item const* item)
{
    return item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) != 0 || item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0;
}

bool HasSocketedGem(Item const* item)
{
    for (uint8 slot = SOCK_ENCHANTMENT_SLOT; slot <= PRISMATIC_ENCHANTMENT_SLOT; ++slot)
    {
        if (slot == BONUS_ENCHANTMENT_SLOT)
            continue;

        uint32 enchantmentId = item->GetEnchantmentId(EnchantmentSlot(slot));
        if (!enchantmentId)
            continue;

        if (SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantmentId))
            if (enchantment->GemID)
                return true;
    }

    return false;
}

bool HasDisenchantEffect()
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(DisenchantSpellId);
    if (!spellInfo)
        return false;

    return std::any_of(
        spellInfo->Effects.begin(), spellInfo->Effects.end(), [](SpellEffectInfo const& effect) { return effect.Effect == SPELL_EFFECT_DISENCHANT; });
}

EligibilityReason ValidatePlayer(Player const* player, PreviewMode mode)
{
    if (!player || !player->IsInWorld())
        return EligibilityReason::PlayerUnavailable;
    if (!player->IsAlive())
        return EligibilityReason::PlayerDead;
    if (player->IsInCombat() && !ArcaneShredderConfig::Instance().AllowInCombat())
        return EligibilityReason::InCombat;
    if (mode == PreviewMode::Disenchant)
    {
        if (!player->HasSpell(DisenchantSpellId) || !HasDisenchantEffect())
            return EligibilityReason::SpellMissing;
        if (!player->HasSkill(SKILL_ENCHANTING))
            return EligibilityReason::SkillMissing;
    }
    else
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(PickLockSpellId);
        if (!player->HasSpell(PickLockSpellId) || !spell ||
            std::none_of(spell->Effects.begin(), spell->Effects.end(), [](SpellEffectInfo const& effect)
            { return effect.Effect == SPELL_EFFECT_OPEN_LOCK && effect.MiscValue == LOCKTYPE_PICKLOCK; }))
            return EligibilityReason::SpellMissing;
        if (!player->HasSkill(SKILL_LOCKPICKING))
            return EligibilityReason::SkillMissing;
    }

    return EligibilityReason::Eligible;
}

EligibilityReason ValidateItem(Player const* player, Item const* item, PreviewFilter const& filter, PreviewMode mode)
{
    EligibilityReason playerReason = ValidatePlayer(player, mode);
    if (playerReason != EligibilityReason::Eligible)
        return playerReason;
    if (!item)
        return EligibilityReason::ItemMissing;
    if (item->GetOwnerGUID() != player->GetGUID())
        return EligibilityReason::NotOwned;

    uint8 bag = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    if (!IsScannableInventoryPosition(bag, slot) || !Player::IsInventoryPos(bag, slot))
        return EligibilityReason::NotInInventory;
    if (item->IsEquipped())
        return EligibilityReason::Equipped;
    if (!IsBagAllowed(filter, bag))
        return EligibilityReason::BagBlocked;
    if (item->IsInTrade())
        return EligibilityReason::InTrade;
    if (item->m_lootGenerated || !item->loot.empty())
        return EligibilityReason::LootInProgress;

    ArcaneShredderConfig const& config = ArcaneShredderConfig::Instance();
    if (item->IsRefundable() && !config.AllowRefundable())
        return EligibilityReason::Refundable;
    if (item->IsBOPTradable() && !config.AllowBopTradeable())
        return EligibilityReason::BopTradeable;
    if (HasRegularEnchantment(item) && !config.AllowEnchanted())
        return EligibilityReason::Enchanted;
    if (HasSocketedGem(item) && !config.AllowSocketed())
        return EligibilityReason::Socketed;

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate)
        return EligibilityReason::TemplateMissing;
    if (mode == PreviewMode::Disenchant)
    {
        if (itemTemplate->Quality < ITEM_QUALITY_UNCOMMON || itemTemplate->Quality > ITEM_QUALITY_EPIC)
            return EligibilityReason::QualityBlocked;
        if ((filter.qualityMask & (1u << itemTemplate->Quality)) == 0)
            return EligibilityReason::QualityBlocked;
        if (itemTemplate->Quality == ITEM_QUALITY_RARE && !config.AllowRare())
            return EligibilityReason::QualityBlocked;
        if (itemTemplate->Quality == ITEM_QUALITY_EPIC && !config.AllowEpic())
            return EligibilityReason::QualityBlocked;
        if (itemTemplate->Class != ITEM_CLASS_WEAPON && itemTemplate->Class != ITEM_CLASS_ARMOR)
            return EligibilityReason::ClassBlocked;
        if (itemTemplate->RequiredDisenchantSkill == uint32(-1) || itemTemplate->RequiredDisenchantSkill > player->GetSkillValue(SKILL_ENCHANTING))
            return EligibilityReason::SkillTooLow;
        if (!itemTemplate->DisenchantID || !LootTemplates_Disenchant.HaveLootFor(itemTemplate->DisenchantID))
            return EligibilityReason::NoLootTemplate;
    }
    else
    {
        if (!itemTemplate->HasFlag(ITEM_FLAG_HAS_LOOT) || !itemTemplate->LockID || !item->IsLocked())
            return item->IsLocked() ? EligibilityReason::NotLockbox : EligibilityReason::AlreadyOpen;
        LockEntry const* lock = sLockStore.LookupEntry(itemTemplate->LockID);
        if (!lock)
            return EligibilityReason::NotLockbox;
        bool pickable = false;
        for (uint8 index = 0; index < MAX_LOCK_CASE; ++index)
        {
            if (lock->Type[index] == LOCK_KEY_SKILL && lock->Index[index] == LOCKTYPE_PICKLOCK
                && player->GetSkillValue(SKILL_LOCKPICKING) >= lock->Skill[index])
                pickable = true;
        }
        if (!pickable)
            return EligibilityReason::SkillTooLow;
    }
    if (config.IsProtected(itemTemplate->ItemId))
        return EligibilityReason::Protected;
    if ((config.MaxItemLevel() && itemTemplate->ItemLevel > config.MaxItemLevel())
        || (mode == PreviewMode::Disenchant && filter.maxItemLevel && itemTemplate->ItemLevel > filter.maxItemLevel))
        return EligibilityReason::ItemLevelBlocked;

    uint32 bindingBit = item->IsSoulBound() ? 0x02 : 0x01;
    if ((filter.bindingMask & bindingBit) == 0)
        return EligibilityReason::BindingBlocked;
    if (item->GetCount() != 1)
        return EligibilityReason::StackUnsupported;

    return EligibilityReason::Eligible;
}

uint32 BuildItemFlags(Item const* item)
{
    uint32 flags = 0;
    if (item->IsSoulBound())
        flags |= 0x01;
    if (item->IsRefundable())
        flags |= 0x02;
    if (item->IsBOPTradable())
        flags |= 0x04;
    if (HasRegularEnchantment(item))
        flags |= 0x08;
    if (HasSocketedGem(item))
        flags |= 0x10;
    return flags;
}

CandidateSnapshot BuildSnapshot(Item const* item)
{
    ItemTemplate const* itemTemplate = item->GetTemplate();
    CandidateSnapshot snapshot;
    snapshot.itemGuid = item->GetGUID();
    snapshot.entry = itemTemplate->ItemId;
    snapshot.count = item->GetCount();
    snapshot.quality = itemTemplate->Quality;
    snapshot.itemLevel = itemTemplate->ItemLevel;
    snapshot.flags = BuildItemFlags(item);
    snapshot.bag = item->GetBagSlot();
    snapshot.slot = item->GetSlot();
    return snapshot;
}

bool MatchesSnapshot(Item const* item, CandidateSnapshot const& snapshot)
{
    ItemTemplate const* itemTemplate = item ? item->GetTemplate() : nullptr;
    return itemTemplate && item->GetGUID() == snapshot.itemGuid && itemTemplate->ItemId == snapshot.entry && item->GetCount() == snapshot.count
        && itemTemplate->Quality == snapshot.quality && itemTemplate->ItemLevel == snapshot.itemLevel;
}

class ArcaneShredderService
{
public:
    static ArcaneShredderService& Instance()
    {
        static ArcaneShredderService instance;
        return instance;
    }

    bool CreatePreview(Player* player, PreviewFilter const& filter, PreviewMode mode, PreviewSession& result, uint32& total, std::string& error)
    {
        if (!ValidateFilter(filter, error))
            return false;
        if (!CheckCooldown(player, error))
            return false;
        if (IsExecuting(player, error))
            return false;

        EligibilityReason reason = ValidatePlayer(player, mode);
        if (reason != EligibilityReason::Eligible)
        {
            error = ReasonCode(reason);
            return false;
        }

        std::vector<CandidateSnapshot> candidates;
        total = 0;
        uint32 limit = ArcaneShredderConfig::Instance().MaxBatchSize();

        auto consider = [&](Item* item)
        {
            if (ValidateItem(player, item, filter, mode) != EligibilityReason::Eligible)
                return;

            ++total;
            if (candidates.size() < limit)
                candidates.push_back(BuildSnapshot(item));
        };

        if ((filter.bagMask & 0x01) != 0)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            uint32 bagBit = 1u << (bagSlot - INVENTORY_SLOT_BAG_START + 1);
            if ((filter.bagMask & bagBit) == 0)
                continue;

            Bag* bag = player->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(uint8(slot)));
        }

        result.ownerGuid = player->GetGUID().GetCounter();
        result.token = GenerateToken();
        result.filter = filter;
        result.mode = mode;
        result.expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(ArcaneShredderConfig::Instance().PreviewLifetimeSeconds());
        result.candidates = std::move(candidates);

        std::lock_guard<std::mutex> lock(_mutex);
        if (_executingPlayers.find(result.ownerGuid) != _executingPlayers.end())
        {
            error = "EXECUTION_IN_PROGRESS";
            return false;
        }
        _previews[result.ownerGuid] = result;
        return true;
    }

    bool Exclude(Player* player, std::string const& token, ObjectGuid::LowType itemGuid, uint32& remaining, std::string& error)
    {
        if (!CheckCooldown(player, error))
            return false;

        std::lock_guard<std::mutex> lock(_mutex);
        PreviewSession* session = FindActiveSession(player, token, error);
        if (!session)
            return false;

        auto candidate = std::find_if(session->candidates.begin(), session->candidates.end(),
            [itemGuid](CandidateSnapshot const& value) { return value.itemGuid.GetCounter() == itemGuid; });
        if (candidate == session->candidates.end())
        {
            error = "ITEM_NOT_IN_PREVIEW";
            return false;
        }

        session->candidates.erase(candidate);
        remaining = session->candidates.size();
        return true;
    }

    bool Cancel(Player* player, std::string const& token, std::string& error)
    {
        if (!CheckCooldown(player, error))
            return false;

        std::lock_guard<std::mutex> lock(_mutex);
        PreviewSession* session = FindActiveSession(player, token, error);
        if (!session)
            return false;

        _previews.erase(player->GetGUID().GetCounter());
        return true;
    }

    bool Consume(Player* player, std::string const& token, PreviewSession& result, std::string& error)
    {
        if (!CheckCooldown(player, error))
            return false;

        std::lock_guard<std::mutex> lock(_mutex);
        ObjectGuid::LowType playerGuid = player->GetGUID().GetCounter();
        if (_executingPlayers.find(playerGuid) != _executingPlayers.end())
        {
            error = "EXECUTION_IN_PROGRESS";
            return false;
        }
        PreviewSession* session = FindActiveSession(player, token, error);
        if (!session)
            return false;
        EligibilityReason reason = ValidatePlayer(player, session->mode);
        if (reason != EligibilityReason::Eligible)
        {
            error = ReasonCode(reason);
            return false;
        }

        result = std::move(*session);
        _previews.erase(playerGuid);
        _executingPlayers.insert(playerGuid);
        return true;
    }

    void FinishExecution(ObjectGuid::LowType playerGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _executingPlayers.erase(playerGuid);
    }

    void ClearPlayer(ObjectGuid::LowType playerGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _previews.erase(playerGuid);
        _lastRequests.erase(playerGuid);
        _executingPlayers.erase(playerGuid);
    }

    void ClearAll()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _previews.clear();
        _lastRequests.clear();
        _executingPlayers.clear();
    }

    bool HasActivePreview(ObjectGuid::LowType playerGuid)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto session = _previews.find(playerGuid);
        if (session == _previews.end())
            return false;
        if (session->second.expiresAt <= std::chrono::steady_clock::now())
        {
            _previews.erase(session);
            return false;
        }
        return true;
    }

private:
    ArcaneShredderService() = default;

    bool ValidateFilter(PreviewFilter const& filter, std::string& error) const
    {
        if ((filter.qualityMask & ~AllowedQualityMask) != 0 || (filter.bindingMask & ~AllowedBindingMask) != 0 || (filter.bagMask & ~AllowedBagMask) != 0
            || filter.maxItemLevel > 1000)
        {
            error = "INVALID_FILTER";
            return false;
        }
        return true;
    }

    bool CheckCooldown(Player const* player, std::string& error)
    {
        if (!player)
        {
            error = "PLAYER_UNAVAILABLE";
            return false;
        }

        uint32 cooldown = ArcaneShredderConfig::Instance().CommandCooldownMilliseconds();
        if (!cooldown)
            return true;

        auto now = std::chrono::steady_clock::now();
        ObjectGuid::LowType playerGuid = player->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(_mutex);
        auto previous = _lastRequests.find(playerGuid);
        if (previous != _lastRequests.end() && now - previous->second < std::chrono::milliseconds(cooldown))
        {
            error = "COOLDOWN";
            return false;
        }

        _lastRequests[playerGuid] = now;
        return true;
    }

    bool IsExecuting(Player const* player, std::string& error)
    {
        if (!player)
        {
            error = "PLAYER_UNAVAILABLE";
            return true;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        if (_executingPlayers.find(player->GetGUID().GetCounter()) == _executingPlayers.end())
            return false;

        error = "EXECUTION_IN_PROGRESS";
        return true;
    }

    PreviewSession* FindActiveSession(Player const* player, std::string const& token, std::string& error)
    {
        if (!player)
        {
            error = "PLAYER_UNAVAILABLE";
            return nullptr;
        }

        ObjectGuid::LowType playerGuid = player->GetGUID().GetCounter();
        auto session = _previews.find(playerGuid);
        if (session == _previews.end())
        {
            error = "TOKEN_NOT_FOUND";
            return nullptr;
        }
        if (session->second.expiresAt <= std::chrono::steady_clock::now())
        {
            _previews.erase(session);
            error = "TOKEN_EXPIRED";
            return nullptr;
        }
        if (session->second.token != token)
        {
            error = "TOKEN_MISMATCH";
            return nullptr;
        }

        return &session->second;
    }

    static std::string GenerateToken()
    {
        std::ostringstream token;
        token << std::hex << std::setfill('0') << std::setw(8) << rand32() << std::setw(8) << rand32();
        return token.str();
    }

    std::mutex _mutex;
    std::unordered_map<ObjectGuid::LowType, PreviewSession> _previews;
    std::unordered_map<ObjectGuid::LowType, std::chrono::steady_clock::time_point> _lastRequests;
    std::unordered_set<ObjectGuid::LowType> _executingPlayers;
};

void SendProtocol(ChatHandler* handler, std::string const& record)
{
    handler->SendSysMessage(record);
}

bool SendFailure(ChatHandler* handler, std::string const& code)
{
    if (handler->IsHumanReadable())
        handler->PSendSysMessage("[ArcaneShredder] Request rejected: {}", code);
    else
        SendProtocol(handler, Acore::StringFormat("ASHRED:ERR:{}", code));

    handler->SetSentErrorMessage(true);
    return true;
}

bool RequireMachineHandler(ChatHandler* handler)
{
    if (!handler->IsHumanReadable())
        return true;

    SendFailure(handler, "ADDON_REQUIRED");
    return false;
}

bool RequireEnabled(ChatHandler* handler)
{
    if (ArcaneShredderConfig::Instance().IsEnabled())
        return true;

    SendFailure(handler, "DISABLED");
    return false;
}

void SendPreview(ChatHandler* handler, PreviewSession const& session, uint32 total)
{
    uint32 ttl = ArcaneShredderConfig::Instance().PreviewLifetimeSeconds();
    SendProtocol(handler, Acore::StringFormat("ASHRED:BEGIN:{}:{}:{}:{}", session.token, ttl, session.candidates.size(),
        session.mode == PreviewMode::Open ? "OPEN" : "DISENCHANT"));
    if (total > session.candidates.size())
        SendProtocol(handler, Acore::StringFormat("ASHRED:WARN:TRUNCATED:{}:{}", total, session.candidates.size()));

    for (CandidateSnapshot const& candidate : session.candidates)
    {
        SendProtocol(handler,
            Acore::StringFormat("ASHRED:ITEM:{}:{}:{}:{}:{}:{}:{}:{}", session.token, candidate.itemGuid.GetCounter(), candidate.entry, candidate.quality,
                candidate.itemLevel, candidate.flags, uint32(candidate.bag), uint32(candidate.slot)));
    }

    SendProtocol(handler, Acore::StringFormat("ASHRED:END:{}:{}", session.token, session.candidates.size()));
}

EligibilityReason ExecuteItem(Player* player, CandidateSnapshot const& candidate, PreviewSession const& session, uint32& mailed)
{
    Item* sourceItem = player->GetItemByGuid(candidate.itemGuid);
    EligibilityReason reason = ValidateItem(player, sourceItem, session.filter, session.mode);
    if (reason == EligibilityReason::Eligible && !MatchesSnapshot(sourceItem, candidate))
        reason = EligibilityReason::ItemChanged;
    if (reason != EligibilityReason::Eligible)
        return reason;

    if (session.mode == PreviewMode::Open)
    {
        LockEntry const* lock = sLockStore.LookupEntry(sourceItem->GetTemplate()->LockID);
        uint32 requiredSkill = 0;
        for (uint8 index = 0; index < MAX_LOCK_CASE; ++index)
        {
            if (lock->Type[index] == LOCK_KEY_SKILL && lock->Index[index] == LOCKTYPE_PICKLOCK
                && player->GetSkillValue(SKILL_LOCKPICKING) >= lock->Skill[index])
            {
                requiredSkill = lock->Skill[index];
                break;
            }
        }

        sourceItem->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_UNLOCKED);
        sourceItem->SetState(ITEM_CHANGED, player);
        player->UpdateGatherSkill(SKILL_LOCKPICKING, player->GetPureSkillValue(SKILL_LOCKPICKING), requiredSkill);
        return EligibilityReason::Eligible;
    }

    ItemTemplate const* sourceTemplate = sourceItem->GetTemplate();
    Loot loot;
    loot.loot_type = LOOT_DISENCHANTING;
    loot.sourceWorldObjectGUID = sourceItem->GetGUID();

    if (!loot.FillLoot(sourceTemplate->DisenchantID, LootTemplates_Disenchant, player, true, true))
        return EligibilityReason::DeliveryFailed;

    uint32 maxSlot = loot.GetMaxSlotInLootFor(player);
    if (!maxSlot)
        return EligibilityReason::DeliveryFailed;

    for (uint32 slot = 0; slot < maxSlot; ++slot)
    {
        LootItem* lootItem = loot.LootItemInSlot(slot, player);
        if (!lootItem || !lootItem->itemid || !lootItem->count)
            return EligibilityReason::DeliveryFailed;
    }

    // Loot generation does not mutate the item, but scripts may run around this
    // command. Resolve and validate it once more at the destructive boundary.
    sourceItem = player->GetItemByGuid(candidate.itemGuid);
    reason = ValidateItem(player, sourceItem, session.filter, session.mode);
    if (reason == EligibilityReason::Eligible && !MatchesSnapshot(sourceItem, candidate))
        reason = EligibilityReason::ItemChanged;
    if (reason != EligibilityReason::Eligible)
        return reason;

    ObjectGuid sourceGuid = sourceItem->GetGUID();
    std::vector<std::pair<LootItem*, ObjectGuid>> storedItems;
    std::vector<std::pair<uint32, uint32>> mailItems;
    for (uint32 slot = 0; slot < maxSlot; ++slot)
    {
        LootItem* lootItem = loot.LootItemInSlot(slot, player);
        ItemPosCountVec destination;
        InventoryResult inventoryResult = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, lootItem->itemid, lootItem->count);
        if (inventoryResult != EQUIP_ERR_OK)
        {
            uint32 itemCount = lootItem->count;
            mailItems.emplace_back(lootItem->itemid, itemCount);
            continue;
        }

        AllowedLooterSet allowedLooters = lootItem->GetAllowedLooters();
        Item* storedItem = player->StoreNewItem(destination, lootItem->itemid, true, lootItem->randomPropertyId, allowedLooters);
        if (!storedItem)
        {
            uint32 itemCount = lootItem->count;
            mailItems.emplace_back(lootItem->itemid, itemCount);
            continue;
        }

        player->SendNewItem(storedItem, lootItem->count, false, false, true);
        storedItems.emplace_back(lootItem, storedItem->GetGUID());
    }

    if (!mailItems.empty())
    {
        player->SendItemRetrievalMail(mailItems);
        mailed += mailItems.size();
    }

    // The command runs synchronously on the player's world thread. No external
    // action can interleave between the final validation above and this removal.
    player->UpdateCraftSkill(DisenchantSpellId);
    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL, DisenchantSpellId);
    player->DestroyItem(sourceItem->GetBagSlot(), sourceItem->GetSlot(), true);

    for (auto const& [lootItem, storedItemGuid] : storedItems)
    {
        player->UpdateLootAchievements(lootItem, &loot);
        if (Item* storedItem = player->GetItemByGuid(storedItemGuid))
            sScriptMgr->OnPlayerLootItem(player, storedItem, lootItem->count, sourceGuid);
    }

    return EligibilityReason::Eligible;
}

ExecutionSummary ExecutePreview(ChatHandler* handler, Player* player, PreviewSession const& session)
{
    ExecutionSummary summary;
    for (CandidateSnapshot const& candidate : session.candidates)
    {
        uint32 mailedBefore = summary.mailed;
        EligibilityReason reason = ExecuteItem(player, candidate, session, summary.mailed);
        if (reason != EligibilityReason::Eligible)
        {
            ++summary.skipped;
            summary.mailed = mailedBefore;
            SendProtocol(handler, Acore::StringFormat("ASHRED:SKIP:{}:{}", candidate.itemGuid.GetCounter(), ReasonCode(reason)));
            if (reason == EligibilityReason::DeliveryFailed)
            {
                LOG_ERROR("module",
                    "ArcaneShredder: loot delivery failed for player {} item {} "
                    "guid {}. Source item retained.",
                    player->GetGUID().ToString(), candidate.entry, candidate.itemGuid.ToString());
            }
            continue;
        }

        ++summary.completed;
    }

    return summary;
}

bool HandleHelloCommand(ChatHandler* handler, uint32 clientProtocol)
{
    if (!RequireMachineHandler(handler) || !RequireEnabled(handler))
        return true;

    SendProtocol(handler, Acore::StringFormat("ASHRED:HELLO:{}:PREVIEW,OPEN,EXCLUDE,CONFIRM,CANCEL,MAIL", ProtocolVersion));
    if (clientProtocol != ProtocolVersion)
        return SendFailure(handler, "PROTOCOL_MISMATCH");
    return true;
}

bool HandlePreviewCommand(ChatHandler* handler, uint32 qualityMask, uint32 maxItemLevel, uint32 bindingMask, uint32 bagMask)
{
    if (!RequireMachineHandler(handler) || !RequireEnabled(handler))
        return true;

    PreviewFilter filter { qualityMask, maxItemLevel, bindingMask, bagMask };
    PreviewSession session;
    uint32 total = 0;
    std::string error;
    if (!ArcaneShredderService::Instance().CreatePreview(handler->GetPlayer(), filter, PreviewMode::Disenchant, session, total, error))
        return SendFailure(handler, error);

    SendPreview(handler, session, total);
    return true;
}

bool HandleOpenPreviewCommand(ChatHandler* handler, uint32 bindingMask, uint32 bagMask)
{
    if (!RequireMachineHandler(handler) || !RequireEnabled(handler))
        return true;

    PreviewFilter filter { 0, 0, bindingMask, bagMask };
    PreviewSession session;
    uint32 total = 0;
    std::string error;
    if (!ArcaneShredderService::Instance().CreatePreview(handler->GetPlayer(), filter, PreviewMode::Open, session, total, error))
        return SendFailure(handler, error);

    SendPreview(handler, session, total);
    return true;
}

bool HandleExcludeCommand(ChatHandler* handler, std::string token, ObjectGuid::LowType itemGuid)
{
    if (!RequireMachineHandler(handler) || !RequireEnabled(handler))
        return true;

    uint32 remaining = 0;
    std::string error;
    if (!ArcaneShredderService::Instance().Exclude(handler->GetPlayer(), token, itemGuid, remaining, error))
        return SendFailure(handler, error);

    SendProtocol(handler, Acore::StringFormat("ASHRED:EXCLUDED:{}:{}:{}", token, itemGuid, remaining));
    return true;
}

bool HandleCancelCommand(ChatHandler* handler, std::string token)
{
    if (!RequireMachineHandler(handler) || !RequireEnabled(handler))
        return true;

    std::string error;
    if (!ArcaneShredderService::Instance().Cancel(handler->GetPlayer(), token, error))
        return SendFailure(handler, error);

    SendProtocol(handler, Acore::StringFormat("ASHRED:CANCELLED:{}", token));
    return true;
}

bool HandleConfirmCommand(ChatHandler* handler, std::string token)
{
    if (!RequireMachineHandler(handler) || !RequireEnabled(handler))
        return true;

    PreviewSession session;
    std::string error;
    if (!ArcaneShredderService::Instance().Consume(handler->GetPlayer(), token, session, error))
        return SendFailure(handler, error);

    ExecutionSummary summary = ExecutePreview(handler, handler->GetPlayer(), session);
    ArcaneShredderService::Instance().FinishExecution(session.ownerGuid);
    SendProtocol(handler, Acore::StringFormat("ASHRED:RESULT:{}:{}:{}:{}", token, summary.completed, summary.skipped, summary.mailed));

    if (ArcaneShredderConfig::Instance().AuditLog())
    {
        LOG_INFO("module",
            "ArcaneShredder: player={} ({}) mode={} token={} preview={} completed={} "
            "skipped={} mailed={}",
            handler->GetPlayer()->GetName(), handler->GetPlayer()->GetGUID().ToString(),
            session.mode == PreviewMode::Open ? "open" : "disenchant", token, session.candidates.size(), summary.completed, summary.skipped,
            summary.mailed);
    }
    return true;
}

bool HandleStatusCommand(ChatHandler* handler)
{
    Player* player = handler->GetPlayer();
    bool enabled = ArcaneShredderConfig::Instance().IsEnabled();
    bool addonChannel = sWorld->getBoolConfig(CONFIG_ADDON_CHANNEL);
    bool hasPreview = player && ArcaneShredderService::Instance().HasActivePreview(player->GetGUID().GetCounter());

    if (handler->IsHumanReadable())
    {
        handler->PSendSysMessage("[ArcaneShredder] enabled={}, addonChannel={}, "
                                 "activePreview={}, protocol={}",
            enabled, addonChannel, hasPreview, ProtocolVersion);
        handler->SendSysMessage("[ArcaneShredder] Use /ashred in the client addon "
                                "for preview, batch disenchant, and batch lockbox unlocking.");
    }
    else
    {
        SendProtocol(handler, Acore::StringFormat("ASHRED:STATUS:{}:{}:{}:{}", enabled ? 1 : 0, addonChannel ? 1 : 0, hasPreview ? 1 : 0, ProtocolVersion));
    }
    return true;
}

class ArcaneShredderWorldScript final : public WorldScript
{
public:
    ArcaneShredderWorldScript()
        : WorldScript("ArcaneShredderWorldScript", { WORLDHOOK_ON_BEFORE_CONFIG_LOAD })
    {
    }

    void OnBeforeConfigLoad(bool reload) override
    {
        ArcaneShredderConfig::Instance().Load(reload);
        if (reload)
            ArcaneShredderService::Instance().ClearAll();
    }
};

class ArcaneShredderPlayerScript final : public PlayerScript
{
public:
    ArcaneShredderPlayerScript()
        : PlayerScript("ArcaneShredderPlayerScript", { PLAYERHOOK_ON_LOGOUT })
    {
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            ArcaneShredderService::Instance().ClearPlayer(player->GetGUID().GetCounter());
    }
};

class ArcaneShredderCommandScript final : public CommandScript
{
public:
    ArcaneShredderCommandScript()
        : CommandScript("ArcaneShredderCommandScript")
    {
    }

    std::vector<ChatCommandBuilder> GetCommands() const override
    {
        static std::vector<ChatCommandBuilder> subCommands
            = { { "hello", HandleHelloCommand, SEC_PLAYER, Console::No }, { "preview", HandlePreviewCommand, SEC_PLAYER, Console::No },
                  { "openpreview", HandleOpenPreviewCommand, SEC_PLAYER, Console::No },
                  { "exclude", HandleExcludeCommand, SEC_PLAYER, Console::No }, { "cancel", HandleCancelCommand, SEC_PLAYER, Console::No },
                  { "confirm", HandleConfirmCommand, SEC_PLAYER, Console::No }, { "status", HandleStatusCommand, SEC_PLAYER, Console::No } };

        static std::vector<ChatCommandBuilder> commands = { { "ashred", subCommands } };
        return commands;
    }
};
} // namespace

void AddSC_mod_arcane_shredder()
{
    new ArcaneShredderWorldScript();
    new ArcaneShredderCommandScript();
    new ArcaneShredderPlayerScript();
}
