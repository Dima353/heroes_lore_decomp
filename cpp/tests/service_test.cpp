#include "save_game.hpp"
#include "service.hpp"
#include "shop_transaction.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static ShopItem gear(int type, int id, int power, int limit, bool ranged) {
    ShopItem item;
    item.type = std::uint8_t(type); item.id = std::uint8_t(id);
    item.class_data.assign(type <= 2 ? 15 : type == 3 ? 13 : 12, 0);
    item.class_data[0] = std::uint8_t(power); item.class_data[1] = std::uint8_t(limit);
    if (ranged) { item.class_data[3] = 2; item.class_data[4] = 1; item.class_data[5] = 3; item.class_data[10] = 2; item.class_data[11] = 2; }
    if (type <= 2) item.class_data[14] = 0xff; // weapon option -1
    if (type == 3) item.class_data[12] = 0xff;
    return item;
}
int main() {
    try {
        const std::vector<ShopItem> catalog{gear(0,1,20,5,true),gear(3,0,4,3,false),gear(5,0,6,4,false)};
        // ad.a(...,true,true): ranged gear rolls two distinct bonuses, starts unidentified.
        InventoryStack sword{0,1,1};
        int calls = 0;
        roll_equipment(sword, catalog[0], true, [&](int lo, int hi) { ++calls; return calls == 1 ? 0 : calls == 3 ? 3 : hi + 0 * lo; });
        check(!sword.identified && sword.bonus[0] == 3 && sword.bonus[3] == 2 && sword.bonus[1] == 0, "random bonuses");

        EquipmentSet equipment{};
        equipment[0] = sword; equipment[2] = InventoryStack{5,0,1};
        equipment[0]->refine = 2; equipment[2]->refine = 3;
        const auto totals = gear_totals(equipment, catalog);
        check(totals.weapon == 25 && totals.defense == 12 && totals.armour_refine == 3 && totals.bonus[0] == 3, "ao.n totals");

        {   // Mixing two Potions out of a full stack: pouring Potion x2 into slot 0 shifts the result.
            std::vector<InventoryStack> bag{{7,0,97},{7,0,2},{7,1,1}};
            const int at = add_item(bag, InventoryStack{7,1,1});
            check(bag.size() == 2 && at == 1 && bag[1].item_id == 1 && bag[1].count == 2, "add_item index after consolidation");
            std::vector<InventoryStack> full{{7,0,99},{7,0,5}};
            check(add_item(full, InventoryStack{7,0,1}) == 1 && full[1].count == 6, "add_item reports the stack with room");
        }
        // Save v3 keeps instance bytes; the refiner's option survives.
        SaveGame save;
        save.switches.assign(128, 0); save.quests.assign(128, 0);
        save.guardians.slots[0] = Guardian{0,1,0,0,-1};
        save.vitals.max_hp = save.vitals.hp = 100; save.vitals.max_sp = save.vitals.sp = 50;
        InventoryStack shield{3,0,1}; shield.option = 4; shield.bonus = {{-2,0,1,0}};
        save.inventory = {sword, shield};
        const auto loaded = decode_save(encode_save(save));
        check(loaded.inventory[0].bonus[0] == 3 && !loaded.inventory[0].identified &&
              loaded.inventory[1].option == 4 && loaded.inventory[1].bonus[0] == -2, "save v3 instance bytes");

        // at: identified gear only; Mithril and gold are taken, a failed roll destroys the piece.
        std::vector<InventoryStack> bag{{11,0,6},{0,1,1}};
        bag[1].identified = true; bag[1].refine = 4;
        std::int64_t money = 1000;
        ServiceInventory inv{bag, equipment, money, catalog};
        ServiceSession smith; smith.kind = ServiceKind::blacksmith;
        const std::function<int(int,int)> fail = [](int, int hi) { return hi; };
        smith.confirm(inv, {}, fail);
        check(smith.page == 0 && smith.message.size() == 4, "refine warning");
        smith.confirm(inv, {}, fail);              // Ok
        smith.confirm(inv, {}, fail);              // open picker
        check(smith.picking && smith.picks.size() == 1 && smith.picks[0] == 1, "refine lists the bag only");
        smith.confirm(inv, {}, fail);
        check(smith.item == 1 && smith.cost == 400 && smith.need == 5, "refine cost");
        smith.navigate(2); smith.confirm(inv, {}, fail);
        check(smith.question && smith.message[0] == 9, "refine question");
        smith.confirm(inv, {}, fail);
        check(smith.busy == 2 && bag[0].count == 1 && smith.item == 1, "Mithril taken");
        smith.finish_refine(inv);
        check(bag.size() == 1 && smith.autosave && smith.message[0] == 26, "failed refine destroys");

        // ab.a(byte): buying wraps 1..99, selling stops at 1 and at the owned count.
        check(step_shop_quantity(1, 3, false, true, 0) == 99 && step_shop_quantity(99, 4, false, true, 0) == 1 &&
              step_shop_quantity(1, 4, false, false, 0) == 1, "buy quantity wraps");
        check(step_shop_quantity(1, 3, true, true, 5) == 1 && step_shop_quantity(5, 4, true, true, 5) == 5 &&
              step_shop_quantity(4, 4, true, true, 5) == 5, "sell quantity does not wrap");

        // k: two Potions mix into a High Potion through itm/mixtbl.
        const auto table = parse_mix_table({2,7,0,7,0,8,0, 2,14,2,7,0,14,5});
        std::vector<InventoryStack> potions{{7,0,2}};
        ServiceInventory mix_inv{potions, equipment, money, catalog};
        ServiceSession refiner; refiner.kind = ServiceKind::refiner; refiner.menu = 1;
        refiner.confirm(mix_inv, table, fail);
        refiner.confirm(mix_inv, table, fail); refiner.confirm(mix_inv, table, fail);
        refiner.navigate(2); refiner.confirm(mix_inv, table, fail); refiner.confirm(mix_inv, table, fail);
        refiner.navigate(2); refiner.confirm(mix_inv, table, fail); refiner.confirm(mix_inv, table, fail);
        check(refiner.message.size() == 1 && refiner.message[0] == 31, "third potion lacks quantity");
        refiner.confirm(mix_inv, table, fail);
        refiner.navigate(2); refiner.confirm(mix_inv, table, fail);
        check(refiner.mix_confirm, "bo confirmation");
        refiner.confirm(mix_inv, table, fail); refiner.confirm(mix_inv, table, fail);
        check(potions.size() == 1 && potions[0].item_type == 8 && refiner.result_view && money == 600, "mixed High Potion");
        std::cout << "service tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
