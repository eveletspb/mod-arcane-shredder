from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SERVER = (ROOT / "src" / "ArcaneShredder.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "conf" / "mod_arcane_shredder.conf.dist").read_text(encoding="utf-8")


class ProtocolContractTest(unittest.TestCase):
    def test_server_exposes_all_protocol_records(self):
        records = (
            "HELLO",
            "BEGIN",
            "WARN",
            "ITEM",
            "END",
            "EXCLUDED",
            "CANCELLED",
            "RESULT",
            "SKIP",
            "STATUS",
            "ERR",
        )
        for record in records:
            with self.subTest(record=record):
                self.assertIn(f"ASHRED:{record}", SERVER)

    def test_machine_actions_require_addon_handler(self):
        for handler in (
            "HandleHelloCommand",
            "HandlePreviewCommand",
            "HandleOpenPreviewCommand",
            "HandleExcludeCommand",
            "HandleCancelCommand",
            "HandleConfirmCommand",
        ):
            start = SERVER.index(f"bool {handler}")
            end = SERVER.index("\n}\n", start)
            self.assertIn("RequireMachineHandler(handler)", SERVER[start:end])

    def test_lockboxes_require_native_pick_lock_skill_and_own_preview(self):
        self.assertIn("player->HasSpell(PickLockSpellId)", SERVER)
        self.assertIn("player->HasSkill(SKILL_LOCKPICKING)", SERVER)
        self.assertIn("player->GetSkillValue(SKILL_LOCKPICKING) >= lock->Skill[index]", SERVER)
        self.assertIn("itemTemplate->HasFlag(ITEM_FLAG_HAS_LOOT)", SERVER)
        self.assertIn("session.mode == PreviewMode::Open", SERVER)
        self.assertIn('"openpreview", HandleOpenPreviewCommand', SERVER)

    def test_unlocking_keeps_lockboxes_and_does_not_generate_loot(self):
        start = SERVER.index("if (session.mode == PreviewMode::Open)", SERVER.index("EligibilityReason ExecuteItem"))
        end = SERVER.index("ItemTemplate const* sourceTemplate", start)
        unlock = SERVER[start:end]
        self.assertIn("sourceItem->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_UNLOCKED)", unlock)
        self.assertIn("sourceItem->SetState(ITEM_CHANGED, player)", unlock)
        self.assertIn("player->UpdateGatherSkill(SKILL_LOCKPICKING", unlock)
        self.assertIn("return EligibilityReason::Eligible", unlock)
        self.assertNotIn("FillLoot", unlock)
        self.assertNotIn("DestroyItem", unlock)

    def test_largest_item_record_fits_azerothcore_addon_envelope(self):
        record = (
            "ASHRED:ITEM:"
            + "F" * 16
            + ":18446744073709551615:4294967295:4:1000:31:255:255"
        )
        envelope = "AzerothCore\t" + "m" + "FFFF" + record
        self.assertLessEqual(len(envelope.encode("utf-8")), 255)

    def test_documented_config_keys_exist(self):
        for key in (
            "Enable",
            "MaxBatchSize",
            "PreviewLifetimeSeconds",
            "CommandCooldownMilliseconds",
            "AllowInCombat",
            "AllowRare",
            "AllowEpic",
            "AllowRefundable",
            "AllowBopTradeable",
            "AllowEnchanted",
            "AllowSocketed",
            "MaxItemLevel",
            "ProtectedItemIds",
            "AuditLog",
        ):
            with self.subTest(key=key):
                self.assertIn(f"ArcaneShredder.{key}", CONFIG)

if __name__ == "__main__":
    unittest.main()
