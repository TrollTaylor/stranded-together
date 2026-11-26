#include "config.hpp"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "config_values.hpp"
#include "give_rules.hpp"
#include "log.hpp"
#include "room_outfit_rules.hpp"

namespace st {
namespace {

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool ParseBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

// "F1".."F12" or a decimal/0x virtual-key code; 0 on anything else.
std::uint32_t ParseKey(const std::string& v) {
    if ((v.size() == 2 || v.size() == 3) && (v[0] == 'F' || v[0] == 'f')) {
        const int n = std::atoi(v.c_str() + 1);
        if (n >= 1 && n <= 12) return 0x70 + static_cast<std::uint32_t>(n - 1);
        return 0;
    }
    const unsigned long k = std::strtoul(v.c_str(), nullptr, 0);
    return k > 0 && k < 0xFF ? static_cast<std::uint32_t>(k) : 0;
}

}  // namespace

const char* RoleName(Role r) {
    switch (r) {
        case Role::host: return "host";
        case Role::guest: return "guest";
        case Role::echo: return "echo";
        case Role::solo: return "solo";
        default: return "none";
    }
}

const char* PartnerBodyName(PartnerBody b) {
    switch (b) {
        case PartnerBody::marker: return "marker";
        case PartnerBody::echo: return "echo";
        case PartnerBody::real: return "real";
        default: return "holo";
    }
}

Config LoadConfig(const std::wstring& dir) {
    Config c;
    std::ifstream in(dir + L"\\session.ini");
    if (!in) {
        Log("[config] no session.ini in mod folder: staying idle");
        return c;
    }
    c.present = true;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        // A UTF-8 BOM (EF BB BF): the same rule as DllMain's EarlyInit and automation::Init (automation.hpp StripBom).
        if (lineno == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
            line = line.substr(3);
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq));
        const std::string v = Trim(line.substr(eq + 1));
        // Round 20 keys, outside the else-if chain below (MSVC's limit on nested blocks: C1061).
        if (k == "fragile_logo") {
            c.fragile_logo = ParseBool(v);
            continue;
        }
        if (k == "enemy_host_hide") {
            c.enemy_host_hide = ParseBool(v);
            continue;
        }
        // Round 22 keys (dev give), outside the chain too (giverules: unit-tested).
        if (k == "dev_give") {
            c.dev_give = ParseBool(v);
            continue;
        }
        if (k == "dev_give_max_kg") {
            c.dev_give_max_kg = giverules::MaxKg(v, c.dev_give_max_kg);
            continue;
        }
        if (k == "dev_give_per_frame") {
            c.dev_give_per_frame = giverules::PerFrame(v, c.dev_give_per_frame);
            continue;
        }
        if (k == "dev_give_slot") {
            c.dev_give_slot = giverules::Slot(v, c.dev_give_slot);
            continue;
        }
        // Round 21 keys (P1c), outside the chain too (cfgval::SetOwnerKey: unit-tested).
        {
            cfgval::OwnerKeys ok{c.enemy_owner_outcomes, c.enemy_follow_guard, c.enemy_attack_registry, c.enemy_owner_hits};
            if (cfgval::SetOwnerKey(ok, k, ParseBool(v))) {
                c.enemy_owner_outcomes = ok.outcomes, c.enemy_follow_guard = ok.follow_guard, c.enemy_attack_registry = ok.attack_registry,
                c.enemy_owner_hits = ok.hits;
                continue;
            }
        }
        // Round 23 keys (cfgval::SetRestrainKey: unit-tested).
        {
            cfgval::RestrainKeys rk{c.enemy_restrain_relay, c.enemy_replay_flush};
            if (cfgval::SetRestrainKey(rk, k, ParseBool(v))) {
                c.enemy_restrain_relay = rk.relay, c.enemy_replay_flush = rk.flush;
                continue;
            }
            cfgval::MirrorKeys mk{c.enemy_mirror_hold_check, c.enemy_mirror_vehicle};
            if (cfgval::SetMirrorKey(mk, k, ParseBool(v))) {
                c.enemy_mirror_hold_check = mk.hold_check, c.enemy_mirror_vehicle = mk.vehicle;
                continue;
            }
            cfgval::TruckKeys tk{c.vehicle_mule_log, c.vehicle_mule_hold};
            if (cfgval::SetTruckKey(tk, k, ParseBool(v))) {
                c.vehicle_mule_log = tk.log, c.vehicle_mule_hold = tk.hold;
                continue;
            }
            if (cfgval::SetDownKey(c.enemy_down_hold, k, ParseBool(v))) continue;
            if (cfgval::SetProbeKey(c.enemy_m2_probes, k, ParseBool(v))) continue;
            {
                cfgval::TakeKeys tk2{c.enemy_take_know, c.enemy_take_know_secs, c.enemy_take_camp, c.enemy_ticket_return,
                                     c.enemy_single_hold_vehicle, c.enemy_single_rule, c.enemy_take_role, c.enemy_take_face};
                if (cfgval::SetTakeKey(tk2, k, v)) {
                    c.enemy_take_know = tk2.know, c.enemy_take_know_secs = tk2.know_secs, c.enemy_take_camp = tk2.camp;
                    c.enemy_ticket_return = tk2.tickets, c.enemy_single_hold_vehicle = tk2.hold_vehicle, c.enemy_single_rule = tk2.rule;
                    c.enemy_take_role = tk2.role, c.enemy_take_face = tk2.face;
                    continue;
                }
            }
            if (k == "enemy_down_hold_radius") {
                c.enemy_down_hold_radius = cfgval::DownRadius(v, c.enemy_down_hold_radius);
                continue;
            }
        }
        if (k == "role") {
            c.role = v == "host"    ? Role::host
                     : v == "guest" ? Role::guest
                     : v == "echo"  ? Role::echo
                     : v == "solo"  ? Role::solo
                                    : Role::none;
        } else if (k == "transport") {
            c.transport = v == "udp" ? Transport::udp : Transport::steam;
        } else if (k == "peer_steamid") {
            c.peer_steam_id = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "secret") {
            c.secret = v;
        } else if (k == "udp_peer") {
            c.udp_peer = v;
        } else if (k == "udp_port_local") {
            c.udp_port_local = static_cast<std::uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "udp_port_peer") {
            c.udp_port_peer = static_cast<std::uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "send_hz") {
            c.send_hz = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "interp_delay_ms") {
            c.interp_delay_ms = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (k == "echo_delay_s") {
            c.echo_delay_s = std::strtof(v.c_str(), nullptr);
        } else if (k == "echo_offset_m") {
            c.echo_offset_m = std::strtof(v.c_str(), nullptr);
        } else if (k == "partner_activate") {
            c.partner_activate = ParseBool(v);
        } else if (k == "partner_body") {
            if (v == "holo" || v == "hologram") c.partner_body = PartnerBody::holo;
            else if (v == "marker") c.partner_body = PartnerBody::marker;
            else if (v == "echo") c.partner_body = PartnerBody::echo;
            else if (v == "real" || v == "solid") c.partner_body = PartnerBody::real;
            else Log("[config] session.ini:%d partner_body '%s' not understood; keeping holo", lineno, v.c_str());
        } else if (k == "partner_holo") {
            // Exactly 8 hex digits = hex, as the mod's log prints HashIds ("0089ABCD"); otherwise
            // C syntax ("0x89ABCD" or decimal).
            const bool hex8 = v.size() == 8 && v.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
            c.partner_holo = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, hex8 ? 16 : 0));
        } else if (k == "partner_marker") {
            c.partner_marker = std::atoi(v.c_str());
            if (c.partner_marker < -1 || c.partner_marker > 255) c.partner_marker = -1;
        } else if (k == "partner_voice") {
            c.partner_voice = ParseBool(v);
        } else if (k == "partner_character") {
            c.partner_character = v;
        } else if (k == "my_character") {
            c.my_character = v;
        } else if (k == "self_character") {
            c.self_character = ParseBool(v);
        } else if (k == "self_fit") {
            c.self_fit = ParseBool(v);
        } else if (k == "self_headgear") {
            c.self_headgear = ParseBool(v);
        } else if (k == "self_feet_hide") {
            c.self_feet_hide = ParseBool(v);
        } else if (k == "textures_dir") {
            c.textures_dir = v;
        } else if (k == "fragile_suit_colour") {
            c.fragile_suit_colour = ParseBool(v);
        } else if (k == "fragile_textures") {
            c.fragile_textures = ParseBool(v);
        } else if (k == "textures_send") {
            c.textures_send = ParseBool(v);
        } else if (k == "textures_receive") {
            c.textures_receive = ParseBool(v);
        } else if (k == "textures_hot_reload") {
            c.textures_hot_reload = ParseBool(v);
        } else if (k == "textures_free") {
            c.textures_free = ParseBool(v);
        } else if (k == "textures_defaults") {
            c.textures_defaults = ParseBool(v);
        } else if (k == "boots_sync") {
            c.boots_sync = ParseBool(v);
        } else if (k == "boots_look") {
            if (v == "own" || v == "1") c.boots_look_own = true;
            else if (v == "prop" || v == "0") c.boots_look_own = false;
            else Log("[config] boots_look=%s is not own / prop: own", v.c_str());
        } else if (k == "boots_own_porter") {
            c.boots_own_porter = ParseBool(v);
        } else if (k == "room_outfit") {
            roomoutfit::Choice ch = roomoutfit::Choice::black;
            if (roomoutfit::ParseChoice(v.c_str(), &ch)) c.room_outfit_black = ch == roomoutfit::Choice::black;
            else Log("[config] room_outfit=%s is not black / suit: black", v.c_str());
        } else if (k == "exo_carrier") {
            c.exo_carrier = ParseBool(v);
        } else if (k == "exo_rig") {
            c.exo_rig = ParseBool(v);
        } else if (k == "exo_rig_extra") {
            c.exo_rig_extra = cfgval::RigExtra(v, c.exo_rig_extra);
        } else if (k == "fragile_hood") {
            c.fragile_hood = v == "2" || v == "unfold" ? 2 : v == "1" || v == "swap" ? 1 : v == "0" || v == "off" ? 0 : (ParseBool(v) ? 2 : 0);
        } else if (k == "hood_hair") {
            c.hood_hair = ParseBool(v);
        } else if (k == "hood_colour") {
            c.hood_colour = ParseBool(v);
        } else if (k == "hood_fold_hide") {
            c.hood_fold_hide = ParseBool(v);
        } else if (k == "waist_cargo") {
            c.waist_cargo = ParseBool(v);
        } else if (k == "cargo_cases_any_slot") {
            c.cargo_cases_any_slot = ParseBool(v);
        } else if (k == "held_items") {
            c.held_items = v == "2" || v == "all" ? 2 : v == "1" || v == "tools" ? 1 : (ParseBool(v) ? 2 : 0);
        } else if (k == "vehicle_sync") {
            c.vehicle_sync = v == "2" ? 2 : v == "3" ? 3 : v == "4" ? 4 : v == "5" || v == "real" ? 5 : (ParseBool(v) ? 1 : 0);
            c.vehicle_sync_set = true;
        } else if (k == "world_sync") {
            c.world_sync = ParseBool(v);
            c.world_sync_set = true;
        } else if (k == "world_anim") {
            c.world_anim = v == "bits" ? 1 : v == "loaded" ? 2 : v == "off" ? 3 : 0;
        } else if (k == "world_contrib") {
            c.world_contrib = ParseBool(v);
        } else if (k == "world_ladders") {
            c.world_ladders = ParseBool(v);
        } else if (k == "world_ziplines") {
            c.world_ziplines = ParseBool(v);
        } else if (k == "world_test_fail") {
            c.world_test_fail = v;
        } else if (k == "vehicle_real_native") {
            c.vehicle_real_native = ParseBool(v);
        } else if (k == "ride_together") {
            c.ride_together = ParseBool(v);
        } else if (k == "ride_hide_pack") {
            c.ride_hide_pack = ParseBool(v);
        } else if (k == "ride_pose") {
            c.ride_pose = ParseBool(v) ? 1 : 0;
        } else if (k == "ride_camera") {
            c.ride_camera = ParseBool(v);
        } else if (k == "partner_vehicle_damage") {
            c.partner_vehicle_damage = ParseBool(v);
        } else if (k == "shots_sync") {
            c.shots_sync = ParseBool(v);
        } else if (k == "shots_muzzle") {
            c.shots_muzzle = ParseBool(v);
        } else if (k == "shots_sound") {
            c.shots_sound = cfgval::ShotsSound(v);
        } else if (k == "shots_friendly_fire") {
            c.shots_friendly_fire = ParseBool(v);
        } else if (k == "shots_npc_drop") {
            c.shots_npc_drop = ParseBool(v);
        } else if (k == "shots_hit_drop") {
            c.shots_hit_drop = ParseBool(v);
        } else if (k == "shots_bt_drop") {
            c.shots_bt_drop = ParseBool(v);
        } else if (k == "enemy_sync") {
            c.enemy_sync = ParseBool(v);
            c.enemy_sync_set = true;
        } else if (k == "enemy_freeze") {
            c.enemy_freeze = cfgval::EnemyFreeze(v);
        } else if (k == "enemy_mirror_radius") {
            c.enemy_mirror_radius = cfgval::MirrorRadius(v, c.enemy_mirror_radius);
        } else if (k == "enemy_pose_count") {
            c.enemy_pose_count = cfgval::PoseCount(v, c.enemy_pose_count);
        } else if (k == "enemy_place_far") {
            c.enemy_place_far = ParseBool(v);
        } else if (k == "enemy_shots") {
            c.enemy_shots = ParseBool(v);
        } else if (k == "enemy_shots_hurt") {
            c.enemy_shots_hurt = ParseBool(v);
        } else if (k == "enemy_shots_sound") {
            c.enemy_shots_sound = ParseBool(v);
        } else if (k == "enemy_mirror_guard") {
            c.enemy_mirror_guard = ParseBool(v);
        } else if (k == "enemy_host_guard") {
            c.enemy_host_guard = ParseBool(v);
        } else if (k == "enemy_entry_err_max") {
            c.enemy_entry_err_max = cfgval::EntryErrMax(v, c.enemy_entry_err_max);
        } else if (k == "enemy_host_far") {
            c.enemy_host_far = cfgval::HostFar(v, c.enemy_host_far);
        } else if (k == "enemy_hits") {
            c.enemy_hits = ParseBool(v);
            c.enemy_hits_set = true;
        } else if (k == "enemy_hit_owner") {
            c.enemy_hit_owner = cfgval::HitOwner(v, c.enemy_hit_owner);
        } else if (k == "enemy_hit_path") {
            c.enemy_hit_path = cfgval::HitPath(v, c.enemy_hit_path);
        } else if (k == "enemy_hits_lethal") {
            c.enemy_hits_lethal = ParseBool(v);
        } else if (k == "enemy_hit_rate") {
            c.enemy_hit_rate = cfgval::HitRate(v, c.enemy_hit_rate);
        } else if (k == "enemy_owner") {
            c.enemy_owner = ParseBool(v);
            c.enemy_owner_set = true;
        } else if (k == "vehicle_overlap_hide") {
            c.vehicle_overlap_hide = ParseBool(v);
        } else if (k == "vehicle_look") {
            c.vehicle_look = ParseBool(v);
        } else if (k == "vehicle_packs") {
            c.vehicle_packs = ParseBool(v);
        } else if (k == "pose_hook") {
            c.pose_hook = ParseBool(v);
        } else if (k == "verbose") {
            c.verbose = ParseBool(v);
        } else if (k == "debug_menu") {
            c.debug_menu = ParseBool(v);
        } else if (k == "dev_tools") {
            c.dev_tools = ParseBool(v);
        } else if (k == "debug_menu_key") {
            const std::uint32_t vk = ParseKey(v);
            if (vk) c.debug_menu_vk = vk;
            else Log("[config] session.ini:%d debug_menu_key '%s' not understood; keeping F9", lineno, v.c_str());
        } else if (k == "automation" || k == "automation_mute" || k == "automation_save_root" || k == "automation_foreign_saves" ||
                   k == "automation_isolate" || k == "automation_exit_trace" || k == "sandbox_foreign_saves" || k == "appdir_settings" ||
                   k == "second_instance") {
            // read by automation.cpp before the one-shot rename (DllMain)
        } else if (k == "automation_run_dir") {
            c.automation_run_dir = v;  // hidden runs: the run's folder (only a textures_dir inside it is read)
        } else if (k == "sandbox_saves") {
            c.sandbox_saves = v;  // the redirect itself is installed in DllMain (automation.cpp)
        } else if (k == "host_save") {
            c.host_save = ParseBool(v);  // a guest's redirect rule is DllMain's (automation.cpp)
        } else if (k == "coop_test_build") {
            c.coop_test_build = v.substr(0, 24);  // hidden tests only (coop_session.cpp checks automation)
        } else if (k == "coop_test_title_off") {
            c.coop_test_title_off = ParseBool(v);  // hidden tests only (coop_session.cpp checks automation)
        } else if (k == "guest_skip_logos") {
            c.guest_skip_logos = ParseBool(v);
        } else if (k == "frame_cap") {
            const unsigned long fc = std::strtoul(v.c_str(), nullptr, 10);
            c.frame_cap = fc == 0 ? 0u : static_cast<unsigned>(fc < 15 ? 15 : fc > 240 ? 240 : fc);
        } else if (k == "local_window") {
            if (v == "left" || v == "right") c.local_window = v;
        } else if (k == "mouse_lock") {
            c.mouse_lock = ParseBool(v);
        } else if (k == "test_net_latency_ms" || k == "test_net_jitter_ms" || k == "test_net_loss_permille" || k == "test_net_burst") {
            unsigned long n = std::strtoul(v.c_str(), nullptr, 10);
            if (n > 1000) n = 1000;  // hidden tests only (link.cpp checks automation)
            (k == "test_net_latency_ms" ? c.test_net_latency_ms : k == "test_net_jitter_ms" ? c.test_net_jitter_ms : k == "test_net_loss_permille" ? c.test_net_loss_permille : c.test_net_burst) =
                static_cast<unsigned>(n);
        } else if (k == "local_input") {
            if (v == "host" || v == "host_pad" || v == "guest") c.local_input = v;
            else Log("[config] session.ini:%d local_input '%s' not understood; ignored", lineno, v.c_str());
        } else if (k == "one_shot") {
            c.one_shot = ParseBool(v);
        } else if (k == "name") {
            c.player_name = v;
        } else if (k == "name_tags") {
            c.name_tags = ParseBool(v);
        } else if (k == "partner_collision") {
            c.partner_collision = ParseBool(v);
        } else if (k == "partner_gear") {
            c.partner_gear = ParseBool(v);
        } else if (k == "smooth_pose") {
            c.smooth_pose = ParseBool(v);
        } else if (k == "audio_probe") {
            c.audio_probe = ParseBool(v);
        } else if (k == "voice_sync") {
            c.voice_sync = ParseBool(v);
        } else if (k == "voice_subtitles") {
            c.voice_subtitles = ParseBool(v);
        } else if (k == "sound_sync") {
            c.sound_sync = ParseBool(v);
        } else if (k == "sound_steps") {
            c.sound_steps = ParseBool(v);
        } else if (k == "vehicle_sound") {
            c.vehicle_sound = ParseBool(v);
        } else if (k == "partner_volume") {
            char* end = nullptr;
            const long pv = std::strtol(v.c_str(), &end, 10);
            if (end == v.c_str() || *end != '\0' || pv < 0 || pv > 100) Log("[config] session.ini:%d partner_volume '%s' ignored (0..100)", lineno, v.c_str());
            else c.partner_volume = static_cast<int>(pv);
        } else if (k == "self_voice") {
            if (!selfvoice::ParseMode(v, &c.self_voice)) Log("[config] session.ini:%d self_voice '%s' not understood (auto, sam or mute); keeping sam", lineno, v.c_str());
        } else if (k == "partner_collision_layer") {
            const int layer = std::atoi(v.c_str());
            if (layer == 9 || layer == 10 || layer == 25) c.partner_collision_layer = layer;
            else Log("[config] partner_collision_layer=%s ignored (25, 10 or 9)", v.c_str());
        } else if (k == "partner_collision_height") {
            const float h = static_cast<float>(std::atof(v.c_str()));
            if (h == 0.0f || (h >= 1.0f && h <= 6.0f)) c.partner_collision_height = h;
            else Log("[config] partner_collision_height=%s ignored (0, or 1 to 6 m)", v.c_str());
        } else if (k == "name_tag_font") {
            if (v == "game" || v == "1") c.name_tag_game_font = true;
            else if (v == "default" || v == "0") c.name_tag_game_font = false;
            else Log("[config] name_tag_font=%s ignored (game or default)", v.c_str());
        } else {
            Log("[config] session.ini:%d unknown key '%s' ignored", lineno, k.c_str());
        }
    }
    in.close();
    if (c.one_shot) {
        const std::wstring from = dir + L"\\session.ini", to = dir + L"\\session.last.ini";
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
            Log("[config] one-shot session: consumed (renamed to session.last.ini)");
        else
            Log("[config] one-shot session: could not rename session.ini (error %lu)", GetLastError());
    }
    if (c.send_hz < 1 || c.send_hz > 120) c.send_hz = 30;
    if (c.interp_delay_ms > 1000) c.interp_delay_ms = 100;
    if (!(c.echo_delay_s >= 0.0f && c.echo_delay_s <= 10.0f)) c.echo_delay_s = 2.0f;
    if (!(c.echo_offset_m >= -50.0f && c.echo_offset_m <= 50.0f)) c.echo_offset_m = 0.0f;
    // Shared structures, ladders and ropes (W3, docs/world-sync.md): on by default in a host-held save session (host_save=1);
    // world_sync=0 turns it off (the launcher's Co-op option "Share structures, ladders and ropes").
    if (!c.world_sync_set) c.world_sync = c.host_save;
    // Enemy sync P1a (docs/enemy-sync.md 8): on by default in a host-held save session as well (round 16b: the mirrors, the
    // host MULEs' gunfire on the guest's screen and the guard proven in run 20261124-124658); enemy_sync=0 turns it off. It needs
    // host_save (its install refuses without it), so the default follows it.
    // Enemy sync P2 (round 17, docs/enemy-sync.md 9): her hits on the host's enemies follow enemy sync (both games need it).
    // Enemy sync P1b (round 17, docs/enemy-sync.md 10): ownership by proximity, off by default this round (enemy_owner=1 turns it
    // on; both games need it: SEH1 parts bit3). The rule is cfgval::EnemyDefaults (tested in st_enemy_rules_tests).
    {
        const cfgval::EnemyKeys k = cfgval::EnemyDefaults(c.host_save, c.enemy_sync_set, c.enemy_sync, c.enemy_hits_set, c.enemy_hits, c.enemy_owner_set,
                                                          c.enemy_owner);
        c.enemy_sync = k.sync, c.enemy_hits = k.hits, c.enemy_owner = k.owner;
    }
    Log("[config] role=%s transport=%s peer=%llu udp=%s:%u->%u send_hz=%u interp=%ums debug_menu=%d key=0x%X",
        RoleName(c.role), c.transport == Transport::udp ? "udp" : "steam",
        static_cast<unsigned long long>(c.peer_steam_id), c.udp_peer.c_str(), c.udp_port_local, c.udp_port_peer,
        c.send_hz, c.interp_delay_ms, c.debug_menu ? 1 : 0, c.debug_menu_vk);
    if (c.host_save)
        Log("[config] host_save=1: host-held save session (%s)", c.role == Role::guest ? "guest: runs only on the redirected co-op folder"
                                                              : c.role == Role::host ? "host: plays on its own saves, offers them read only"
                                                                                     : "ignored for this role");
    if (!c.sandbox_saves.empty() || !c.local_input.empty())
        Log("[config] test save sandbox=%s local_input=%s", c.sandbox_saves.empty() ? "-" : c.sandbox_saves.c_str(),
            c.local_input.empty() ? "-" : c.local_input.c_str());
    if (c.mouse_lock) Log("[config] mouse_lock=1: the cursor stays inside the game window that is in front");
    Log("[config] partner_body=%s partner_holo=%08X partner_marker=%d partner_voice=%d partner_character=%s my_character=%s "
        "self_character=%d self_fit=%d pose_hook=%d",
        PartnerBodyName(c.partner_body), c.partner_holo, c.partner_marker, c.partner_voice ? 1 : 0,
        c.partner_character.empty() ? "-" : c.partner_character.c_str(), c.my_character.empty() ? "-" : c.my_character.c_str(),
        c.self_character ? 1 : 0, c.self_fit ? 1 : 0, c.pose_hook ? 1 : 0);
    Log("[config] name_tags=%d font=%s name=%s", c.name_tags ? 1 : 0, c.name_tag_game_font ? "game" : "default", c.player_name.empty() ? "- (Steam name)" : "set");
    Log("[config] partner_collision=%d layer=%d height=%.2f", c.partner_collision ? 1 : 0, c.partner_collision_layer,
        static_cast<double>(c.partner_collision_height));
    Log("[config] partner_gear=%d smooth_pose=%d self_headgear=%d self_feet_hide=%d boots_sync=%d boots_look=%s boots_own_porter=%d fragile_suit_colour=%d exo_carrier=%d "
        "room_outfit=%s fragile_hood=%d hood_hair=%d",
        c.partner_gear ? 1 : 0, c.smooth_pose ? 1 : 0, c.self_headgear ? 1 : 0, c.self_feet_hide ? 1 : 0, c.boots_sync ? 1 : 0, c.boots_look_own ? "own" : "prop",
        c.boots_own_porter ? 1 : 0, c.fragile_suit_colour ? 1 : 0, c.exo_carrier ? 1 : 0, c.room_outfit_black ? "black" : "suit",
        c.fragile_hood, c.hood_hair ? 1 : 0);
    Log("[config] hood_colour=%d hood_fold_hide=%d fragile_logo=%d", c.hood_colour ? 1 : 0, c.hood_fold_hide ? 1 : 0, c.fragile_logo ? 1 : 0);
    Log("[config] held_items=%d (%s)", c.held_items,
        c.held_items == 2 ? "every item in a hand drawn on the other screen" : c.held_items == 1 ? "ladder, PCC, climbing anchor" : "off: cutscene-prop weapons only");
    Log("[config] waist_cargo=%d cargo_cases_any_slot=%d", c.waist_cargo ? 1 : 0, c.cargo_cases_any_slot ? 1 : 0);
    Log("[config] exo_rig=%d exo_rig_extra=%d", c.exo_rig ? 1 : 0, c.exo_rig_extra);
    Log("[config] shots_sync=%d shots_muzzle=%d shots_sound=%d shots_friendly_fire=%d shots_npc_drop=%d shots_hit_drop=%d shots_bt_drop=%d", c.shots_sync ? 1 : 0,
        c.shots_muzzle ? 1 : 0, c.shots_sound, c.shots_friendly_fire ? 1 : 0, c.shots_npc_drop ? 1 : 0, c.shots_hit_drop ? 1 : 0, c.shots_bt_drop ? 1 : 0);
    Log("[config] enemy_sync=%d%s freeze=%s radius=%.0f poses=%d place_far=%d shots=%d hurt=%d sound=%d guard=%d", c.enemy_sync ? 1 : 0,
        c.enemy_sync_set ? "" : " (default: on with host_save=1)", c.enemy_freeze == 1 ? "occupied" : "not built (off)", c.enemy_mirror_radius, c.enemy_pose_count,
        c.enemy_place_far ? 1 : 0, c.enemy_shots ? 1 : 0, c.enemy_shots_hurt ? 1 : 0, c.enemy_shots_sound ? 1 : 0, c.enemy_mirror_guard ? 1 : 0);
    Log("[config] enemy_host_guard=%d enemy_entry_err_max=%.0f enemy_host_far=%.0f enemy_host_hide=%d", c.enemy_host_guard ? 1 : 0, c.enemy_entry_err_max,
        c.enemy_host_far, c.enemy_host_hide ? 1 : 0);
    Log("[config] enemy_hits=%d%s owner=%s path=%s lethal=%d rate=%d", c.enemy_hits ? 1 : 0, c.enemy_hits_set ? "" : " (default: on with enemy_sync)",
        cfgval::HitOwnerName(c.enemy_hit_owner), c.enemy_hit_path == 1 ? "query" : "direct", c.enemy_hits_lethal ? 1 : 0, c.enemy_hit_rate);
    Log("[config] enemy_owner=%d%s outcomes=%d hits=%d follow_guard=%d attack_registry=%d", c.enemy_owner ? 1 : 0,
        c.enemy_owner_set ? "" : cfgval::kOwnerDefaultOn ? " (default: on with enemy_hits)" : " (default: off)", c.enemy_owner_outcomes ? 1 : 0,
        c.enemy_owner_hits ? 1 : 0, c.enemy_follow_guard ? 1 : 0, c.enemy_attack_registry ? 1 : 0);
    Log("[config] enemy_restrain_relay=%d enemy_replay_flush=%d", c.enemy_restrain_relay ? 1 : 0, c.enemy_replay_flush ? 1 : 0);
    Log("[config] enemy_mirror_hold_check=%d enemy_mirror_vehicle=%d", c.enemy_mirror_hold_check ? 1 : 0, c.enemy_mirror_vehicle ? 1 : 0);
    Log("[config] vehicle_mule_log=%d vehicle_mule_hold=%d", c.vehicle_mule_log ? 1 : 0, c.vehicle_mule_hold ? 1 : 0);
    Log("[config] enemy_down_hold=%d enemy_down_hold_radius=%.0f", c.enemy_down_hold ? 1 : 0, c.enemy_down_hold_radius);
    Log("[config] enemy_m2_probes=%d", c.enemy_m2_probes ? 1 : 0);
    Log("[config] enemy_take_know=%d enemy_take_know_secs=%.0f enemy_take_camp=%d enemy_ticket_return=%d enemy_single_hold_vehicle=%d "
        "enemy_single_rule=%d enemy_take_role=%d enemy_take_face=%d",
        c.enemy_take_know, c.enemy_take_know_secs, c.enemy_take_camp, c.enemy_ticket_return ? 1 : 0, c.enemy_single_hold_vehicle ? 1 : 0,
        c.enemy_single_rule ? 1 : 0, c.enemy_take_role ? 1 : 0, c.enemy_take_face ? 1 : 0);
    // Fragile's textures: only whether a folder came and its length (the path itself names the Windows user).
    Log("[config] textures_dir=%s", c.textures_dir.empty() ? "- (none)" : ("set (" + std::to_string(c.textures_dir.size()) + " bytes)").c_str());
    Log("[config] fragile_textures=%d textures_send=%d textures_receive=%d textures_hot_reload=%d textures_free=%d textures_defaults=%d", c.fragile_textures ? 1 : 0,
        c.textures_send ? 1 : 0, c.textures_receive ? 1 : 0, c.textures_hot_reload ? 1 : 0, c.textures_free ? 1 : 0, c.textures_defaults ? 1 : 0);
    Log("[config] vehicle_sync=%d%s vehicle_overlap_hide=%d vehicle_look=%d vehicle_packs=%d vehicle_real_native=%d", c.vehicle_sync,
        c.vehicle_sync_set ? "" : " (default: 5 real with host_save=1 when its checks pass, else 1)", c.vehicle_overlap_hide ? 1 : 0,
        c.vehicle_look ? 1 : 0, c.vehicle_packs ? 1 : 0, c.vehicle_real_native ? 1 : 0);
    Log("[config] ride_together=%d ride_pose=%d ride_camera=%d ride_hide_pack=%d partner_vehicle_damage=%d", c.ride_together ? 1 : 0, c.ride_pose,
        c.ride_camera ? 1 : 0, c.ride_hide_pack ? 1 : 0, c.partner_vehicle_damage ? 1 : 0);
    Log("[config] world_sync=%d%s contrib=%d ladders=%d ziplines=%d anim=%s%s%s", c.world_sync ? 1 : 0, c.world_sync_set ? "" : " (default: on with host_save=1)",
        c.world_contrib ? 1 : 0, c.world_ladders ? 1 : 0, c.world_ziplines ? 1 : 0,
        c.world_anim == 1 ? "bits" : c.world_anim == 2 ? "loaded" : c.world_anim == 3 ? "off" : "645", c.world_test_fail.empty() ? "" : " test_fail=",
        c.world_test_fail.c_str());
    if (c.audio_probe) Log("[config] audio_probe=1 (logging only)");
    Log("[config] self_voice=%s voice_sync=%d voice_subtitles=%d sound_sync=%d sound_steps=%d partner_volume=%d vehicle_sound=%d",
        selfvoice::ModeName(c.self_voice), c.voice_sync ? 1 : 0, c.voice_subtitles ? 1 : 0, c.sound_sync ? 1 : 0, c.sound_steps ? 1 : 0, c.partner_volume,
        c.vehicle_sound ? 1 : 0);

    return c;
}

}  // namespace st
