// Session configuration: <game>\stranded_together\session.ini, written by the
// launcher (or by hand for testing). Missing file = mod idle (game runs normally).
// With one_shot=1 (what the launcher writes) the mod renames the file to
// session.last.ini as soon as it has read it, so the next plain Steam launch of
// the game runs unmodded.
#pragma once
#include <cstdint>
#include <string>

#include "self_voice_setting.hpp"

namespace st {

// solo: debug tools only (in-game menu, logging), no partner and no network.
enum class Role { none, host, guest, echo, solo };
enum class Transport { steam, udp };
// What the partner looks like (docs/partner-hologram.md, docs/real-partner-body.md).
// Each body falls back to the next one when it cannot be found or spawned:
// real -> holo -> marker -> echo. `real` is a solid character body that plays the
// other player's real animation (experimental; not the default yet).
enum class PartnerBody { holo, marker, echo, real };

struct Config {
    bool present = false;          // session.ini existed and parsed
    Role role = Role::none;
    Transport transport = Transport::steam;

    // Steam transport
    std::uint64_t peer_steam_id = 0;   // guest: host's SteamID64 (0 = wait for invite)
    std::string secret;                // shared join secret (from the join code)

    // UDP transport (LAN / same-PC testing without Steam networking)
    std::string udp_peer = "127.0.0.1";
    std::uint16_t udp_port_local = 27555;
    std::uint16_t udp_port_peer = 27556;

    // Sync tuning
    std::uint32_t send_hz = 30;
    std::uint32_t interp_delay_ms = 100;
    float echo_delay_s = 2.0f;         // echo role: replay own movement this many seconds behind
    float echo_offset_m = 0.0f;        // echo role: extra sideways offset

    // Partner
    bool partner_activate = true;      // wake the spawned clone (gated: never touches the real Sam); 0 = leave it asleep
    PartnerBody partner_body = PartnerBody::holo;  // partner_body=holo|marker|echo
    std::uint32_t partner_holo = 0;    // collectors item HashId to show (hex or decimal); 0 = automatic
    int partner_marker = -1;           // trace-point prop index for the marker body; -1 = first usable
    bool partner_voice = false;        // also spawn the (invisible) echo next to a holo/marker body as its voice source
    // partner_body=real, debug override: show this body_table key (sam, porter, fragile, ...) for the
    // partner instead of the one the peer sends (STJ1 body_id). "" = follow the peer (the normal case).
    std::string partner_character;
    // The character THIS player appears as on the other player's screen (body_table key), sent as
    // STJ1 body_id. "" / unknown = sam; the host always sends sam (docs/real-partner-body.md).
    std::string my_character;
    // Self view: on THIS player's own screen the local Sam is replaced by my_character too (a
    // guest whose character is not sam; the host is always Sam). Written by the launcher
    // (self_character=1, Debug option, default on); off when absent. Needs the pose hook
    // (partner_body=real or pose_hook=1). docs/real-partner-body.md section 8.
    bool self_character = false;
    // Self view fit: while the self body is shown, Sam's hidden skeleton takes the character's bone
    // lengths, so the gear, cargo and boots the game hangs on it sit on the character
    // (docs/real-partner-body.md section 16). Default on; self_fit=0 keeps Sam's proportions.
    bool self_fit = true;
    // Self view headgear (docs/real-partner-body.md section 20): with the fit on, Sam's cap and glasses get an offset of
    // their own in his head helper's frame (the accessory's own mover), made for the character's head, instead of the
    // head lift. Default on; self_headgear=0 keeps the v0.11 head lift.
    bool self_headgear = true;
    // Self view feet (section 21): her own feet are scaled to nothing under Sam's equipped boots. Default on;
    // self_feet_hide=0 keeps her own boots under his.
    bool self_feet_hide = true;
    // v0.23: Fragile's porter suit (fragile_suit) in the suit colour of the player she stands for (that player's own Sam's
    // porter-suit colour: the palette, silver, gold), on both screens (docs/real-partner-body.md section 27). Round 6: the
    // fallback when that colour has no edited textures; default off (her own navy suit); 1 = the v0.23 tint.
    bool fragile_suit_colour = false;
    // Fragile's textures (docs/textures.md): the folder of the player's edited PNGs, as the launcher writes it (UTF-8).
    // The mod only reads it (its built textures are cached in its own cache\built-textures). Hidden runs: only a folder inside the run's own folder.
    std::string textures_dir;
    std::string automation_run_dir;  // hidden runs: the run's folder the supervisor names (textures_dir must lie inside it)
    // Round 6 T2 (docs/textures.md, docs/real-partner-body.md section 29): the guest's edited pictures on Fragile's suit, by
    // her Sam suit colour, on both screens. 0 = no loader, no detour, no texture messages. send: the guest sends its look to
    // the host; receive: the host takes it; hot_reload: a saved PNG shows again a few seconds later.
    bool fragile_textures = true;
    bool textures_send = true;
    bool textures_receive = true;
    bool textures_hot_reload = true;
    bool textures_free = true;  // a texture of ours unused for 5 s is released (0: kept for the session, at most 48)
    // Round 8 (docs/textures.md "Ready-made colours"): a colour folder 01..16 with nothing painted wears Stranded
    // Together's ready-made colour (made from her Original pictures); 0: her own suit there (the v0.24 rule).
    bool textures_defaults = true;
    // Partner boots (section 21): the sender's equipped boots as a gear item (STC1 kind 7) and, on the other screen, a
    // cutscene boots prop posed from the partner body's legs with the partner's own feet scaled away. Default on;
    // boots_sync=0: nothing read, sent or spawned (the partner shows its own model's boots).
    bool boots_sync = true;
    // Round 6 T3 (docs/real-partner-body.md section 30, docs/textures.md): how her boots look on both screens. boots_look_own
    // (session.ini boots_look=own, the default): her own model's porter boots, painted with the user's pictures of the guest's
    // boot type; no boots prop, her feet never scaled, Sam's boots entity hidden under the self view. boots_look=prop: Sam's
    // boots on her feet (v0.22). boots_own_porter (prop only, default on): sole grass and intact porter boots keep her own.
    bool boots_look_own = true;
    bool boots_own_porter = true;
    // Round 7 (docs/real-partner-body.md section 32): Sam's skeleton (exoskeleton) on a body that is not Sam (Fragile), on
    // both screens: an "exo carrier" (a demo_sam_clean body showing only the skeleton pieces) posed from her body. Default
    // on; exo_carrier=0: no carrier (her body shows no skeleton, as before).
    bool exo_carrier = true;
    // Round 20 (docs/real-partner-body.md section 41): the suit rig on that carrier, on both screens: the porter suit's hardware
    // that joins her gear to her body (the BB pod's chest mount and pipe, the backpack's straps and hip belt on the partner's
    // screen, the hip rig) while the sender, in the porter suit, wears the pod, the backpack or a piece on the hips. Default
    // on; exo_rig=0: as round 19 (the pod and the frame with nothing between them and her). exo_rig_extra (default 0): the
    // optional parts, a mask (1 legbelt, 2 cantmount, 4 subpouch, 8 bodyparts), for looks only.
    bool exo_rig = true;
    int exo_rig_extra = 0;
    // Round 11 (docs/real-partner-body.md section 34): what Fragile (fragile_suit) wears in the private room, on both screens.
    // room_outfit=black (default): her black suit (demo_fgl_new, body 3 "fragile"), changed while the game changes Sam's
    // clothes (or on the load into the room) and back to the porter suit on leaving; suit: the porter suit there too
    // (v0.25). The launcher's Tools > Fragile's textures "FRAGILE IN THE PRIVATE ROOM".
    bool room_outfit_black = true;
    // Round 11 (docs/real-partner-body.md section 37): her rain hood, on both screens, while Sam's hood is up (timefall): the
    // exo carrier draws Sam's hood of the same type on her. fragile_hood=2 (default): with the game's own unfold where the
    // carrier can play it (else as 1); 1: the timed swap (the raised hood appears when Sam's is up); 0: off (v0.25: no hood,
    // her hat stays on in the rain). hood_hair=1 (default): her hair hidden while her hood is drawn; 0: shown.
    int fragile_hood = 2;
    bool hood_hair = true;
    // Round 12 (sections 37.5, 37.6): hood_colour=1 (default): the carrier dressed in the colour her suit wears (her hood
    // matches her suit); 0: in the sender's Sam's colour (v0.26). Round 13 (section 37.8): hood_fold_hide=1 (default): her
    // folded hood (the roll on her collar, mesh D of her suit) hidden while the raised hood shows, per body, through the
    // view mask of its slot in that body's own mesh instance tree (her LOD0 pinned meanwhile); 0: not hidden (v0.27).
    bool hood_colour = true;
    bool hood_fold_hide = true;
    // Round 20 (docs/textures.md "The Fragile Express logo", real-partner-body.md section 42): fragile_logo=1 (default): the
    // BRIDGES print on her raised hood (the exo carrier's Sam hood) and on the Bridges cap she wears replaced by the FRAGILE
    // express wordmark, through copies each game makes from its own extracted pictures (Logo (reference)); the guest's
    // choice rides its LOOK (flags bit 2) so the host draws the same. 0: BRIDGES (v0.32). Sam's own hood and cap never change.
    bool fragile_logo = true;
    // Round 6 (docs/real-partner-body.md section 31): what is in a player's hand on the other screen. held_items (0 off: only
    // the cutscene-prop weapons of v0.9; 1 the ladder, PCC and climbing anchor; 2, the default, every weapon / tool without a
    // cutscene prop) is drawn as a plain entity made from the item's own resource ("held clone").
    int held_items = 2;
    // Round 14 (docs/real-partner-body.md section 39): the hip crates (the weapon case and the folding ladder Sam carries on his
    // waist slots) sent and drawn on the partner as the game's own baggage model. On by default since round 15 (39.6: the model's
    // representation is given its message handlers, so the crates are drawn); 0: v0.28 (not sent or drawn).
    bool waist_cargo = true;
    // Round 20 (F2, section 39.8): the weapon cases and the folding ladder stacked on the back sent as well, still under
    // waist_cargo (the arms and hands not: never seen drawn). 0: the hips only (v0.32).
    bool cargo_cases_any_slot = true;
    // Vehicles (docs/real-partner-body.md section 22): the vehicle the player rides is sent (STM1) and shown under the
    // partner's body on the other screen as a visual copy. Section 22.11 (after v0.13): 1 = on, SOLID trike and truck
    // copies (default; each copy's collision turned off on its own entity), the roadster as its construction hologram (no
    // solid roadster copy exists); 2 = the same (v0.13's value for the solid copies); 3 = all three as the garage's
    // construction holograms (opt-in; v0.13's default look); 4 = the roadster's copy only (the v0.12 default); 0 =
    // nothing read, sent or spawned. Every value but 0 sends what we ride. 5 ("real", design M2; docs/real-partner-body.md
    // section 23): one real game vehicle per vehicle in both worlds (vehicle_share.cpp), no copies; the DEFAULT when the key
    // is absent and host_save=1 and its byte checks pass. Without host_save, or when a check fails, the copies of 1 stand
    // in, and the defaults (1, 2) show no roadster hologram (decision D2-1: "no hologram vehicles"); a solid
    // copy that cannot be made safe shows nothing. 3 and 4 stay the opt-in hologram modes.
    int vehicle_sync = 1;
    bool vehicle_sync_set = false;     // the key was in session.ini (else the default above decides)
    // vehicle_sync=5 only (docs/real-partner-body.md section 23.16): 1 = the real follower vehicle's lamps and engine / road
    // sounds are the game's own only (the NPC driver controller's engine); default 0 keeps the v0.14 lamp writes and the
    // STA1 vehicle-sound mirror on the real vehicle until a hidden run proves the native ones.
    bool vehicle_real_native = false;
    // Shared structures (world sync, docs/world-sync.md), with host_save=1: structures either player builds with the PCC
    // appear in both worlds and stay in the host's save. W3: on by default when host_save=1 (key absent); world_sync=0 turns it off.
    // Both games must have it on (HELLO). world_anim=645 (default: the game's own new-build path) | loaded | off.
    // world_test_fail=<byte site name part>: hidden runs only, treat that site as failed (the off path's test).
    bool world_sync = false;
    bool world_sync_set = false;
    int world_anim = 0;                // 0 "645", 1 bits (not in W1: as 645), 2 loaded, 3 off
    // world_contrib=0/1 (default 1 when world_sync is on): contributions, upgrades, repair and durability (W2). Both games
    // must have it on (HELLO parts bit1); off on either side = each world keeps its own numbers.
    bool world_contrib = true;
    // world_ladders=0/1 (default 1 when world_sync is on): ladders and climbing anchors / ropes either player places or picks
    // up (W3). Both games must have it on (HELLO parts bit2); off on either side = each world keeps its own.
    bool world_ladders = true;
    // world_ziplines=0/1 (default 1 when world_sync is on): ziplines either player builds, upgrades or dismantles (W4). Both
    // games must have it on (HELLO parts bit3); off on either side = each world keeps its own.
    bool world_ziplines = true;
    std::string world_test_fail;
    // Riding together (design M3; docs/real-partner-body.md section 24), with vehicle_sync=5: hold X (pad) / F (keyboard) 0.6 s
    // beside the partner's vehicle to ride as its passenger (the trike: a pillion). Default on; 0 = off on this game (its
    // driver's game denies requests, nothing is offered).
    bool ride_together = true;
    // The carried passenger's pose (section 24.14): 1 (default) the seated idle (a held seated base of the vehicle kind, the
    // hands resting on the thighs); 0 = v0.20's copy of the driver's live pose (steering, leaning with the driver).
    int ride_pose = 1;
    // v0.23: the trike's driver with the partner on the pillion behind him: his backpack and its stacked cargo are hidden
    // (the game's own HideBackpack, both screens) and shown again when the ride ends. Default on; 0 = shown as in v0.22.
    bool ride_hide_pack = true;
    // The carried passenger's camera (section 24.15): 1 (default) the game's own ride camera of the vehicle he rides (the
    // driver's camera); 0 = v0.20's on-foot camera on the carried Sam.
    bool ride_camera = true;
    // Vehicle hits (design M4; docs/real-partner-body.md section 25), with vehicle_sync=5: the partner's vehicle hits our Sam
    // like an NPC-driven truck (the game's own vehicle attack: damage and the Blow knock-down, never an instant kill), and a
    // passenger thrown off in a crash takes the same hit. Default on; 0 = the partner's vehicles never hurt THIS player's Sam
    // (each game decides for its own Sam; the launcher's Debug page "Partner's vehicles can knock me down").
    bool partner_vehicle_damage = true;
    // Shots (docs/enemy-sync.md P3a, "ship A", 7.5; shot_sync.hpp): a player's gunfire shown on the other screen through the
    // game's own CreateBullet (muzzle flash, trail, impacts). shots_sync: capture, send and replay. Default ON since round 16:
    // a replay is visual only, its hits are dropped at the bullet (shots_hit_drop) before any vehicle, cargo, structure or NPC
    // of the watcher's world takes them; the launcher writes only shots_sync=0 (its Debug page "Show my partner's gunfire"
    // unticked), so the mod's defaults hold otherwise.
    // shots_muzzle: pass the weapon id (the muzzle flash; 0 = trails and impacts only). shots_sound: pass the magazine id
    // (the magazine's effect at the muzzle: a gunshot-like event sharing the weapon sound's RTPC, run 20261124-092442).
    // Default 1: sound_sync plays the partner's own gunshot only once this game posted that event itself (partner_sound's
    // learned allow-list), so without it the partner's gunfire would often be silent; "auto" (only while sound_sync=0) is
    // the opt-in for the two-game sound check; docs/enemy-sync.md 7.5. shots_friendly_fire: 1 = the
    // partner's replayed bullets can hurt our Sam (default 0: dropped). shots_npc_drop: the backstop on the Humanoid damage
    // handler (MULEs; default on; while it is on but not installed, nothing is replayed). shots_hit_drop: the drop at the
    // bullet (BulletDamageA / B; default on; while it is on but not installed, nothing is replayed; 0 = round 15's behaviour:
    // only our Sam and Humanoids filtered; it also carries enemy sync's guard on frozen mirrors).
    bool shots_sync = true;
    bool shots_muzzle = true;
    int shots_sound = 1;  // -1 auto (on while sound_sync=0), 0 off, 1 on
    bool shots_friendly_fire = false;
    bool shots_npc_drop = true;
    bool shots_hit_drop = true;
    // shots_bt_drop: the BT reaction's drop (a detour on the hit processor and the BT reaction; default 0 until a run against a BT
    // proves it: without it a replay that hits a BT may make it react; review 16).
    bool shots_bt_drop = false;
    // Enemy sync P1a (docs/enemy-sync.md 8; enemy_sync.hpp): the host owns every enemy; the guest freezes its own copies near her
    // (MsgGameActorSequenceOccupied), drives them to the host's places (SEN1) and poses the nearest from the host's joints (SEP1),
    // and the host MULEs' gunfire is replayed on her screen from the mirrors' guns (enemy_shots). Both games need it, with equal
    // enemy fingerprints. enemy_sync: default on with host_save=1 (round 16b; enemy_sync_set: the key was given). enemy_freeze: occupied (the
    // only lever built). enemy_mirror_radius: the 10 Hz class and the mirrors (m). enemy_pose_count: 0..8 posed mirrors (0 no
    // SEP1). enemy_place_far: the guest's copies out of the world placed at the host's places (variant A). enemy_shots: the enemy
    // shots (installs the shots' hooks even with shots_sync=0). enemy_shots_hurt: an enemy round replayed on the guest may hurt
    // her own Sam (the victim decides). enemy_shots_sound: pass the magazine id on an enemy muzzle shot. enemy_mirror_guard: the
    // frozen mirrors' hits on our Sam, hits on them and their own fire dropped (0 = counted only, diagnosis). The launcher writes
    // none of these keys.
    bool enemy_sync = false;
    bool enemy_sync_set = false;
    int enemy_freeze = 1;  // 1 occupied
    double enemy_mirror_radius = 250.0;
    int enemy_pose_count = 6;
    bool enemy_place_far = true;
    bool enemy_shots = true;
    bool enemy_shots_hurt = true;
    bool enemy_shots_sound = true;
    bool enemy_mirror_guard = true;
    // M0 "no statues" (round 20, docs/enemy-sync.md 8.10): enemy_host_guard: a copy the host has unloaded or hidden is never
    // mirrored (entry refused, a mirror released alive). enemy_host_hide: a mirror released because the host hid its copy is hidden
    // and put to sleep here too while hidden there (default 0: never run yet; review r20 enemy 3). enemy_entry_err_max: no mirror
    // when the host's place is more than this far from ours at entry (m; 0 off, the default: M0's own releases let her MULEs
    // drift from the host's, and the refusal would then never let them follow his again; review r20 enemy 2). The launcher writes
    // none of them.
    bool enemy_host_guard = true;
    bool enemy_host_hide = false;
    double enemy_entry_err_max = 0.0;
    // enemy_host_far (M0's quick path): with the guard on, a mirror is given back (and none made) while the host's own Sam is more
    // than this far from it (m; 0 off): after his teleport his frames for it stop while his world loads, the silence window
    // would take 3 s.
    double enemy_host_far = 1500.0;
    // Enemy sync P2 (round 17, docs/enemy-sync.md 9; enemy_combat.hpp): her hits on the enemies the host owns, applied there with the
    // game's own damage path, and the outcomes (faint, sleep, restrained, dead) back on her mirrors. enemy_hits: default on with
    // enemy_sync (enemy_hits_set: the key was given; both games need it: SEH1 parts bit1). enemy_hit_owner: whose attack the host
    // applies (0 credit: the partner body, the player's attacker id stamped so the takedown counts as the host's; 1 body; 2 none;
    // 3 sam: the falloff from the host's place). enemy_hit_path: 0 direct (M1, the bullet's path), 1 query (M2, the attack sphere
    // query). enemy_hits_lethal: 1 lets her lethal rounds (the attack resource's damage > 0) through; default 0 until a lethal run
    // passes (the kill would count as the host's, in the host's save): refused at the guest by hers and at the host by the host's
    // (HITRES `lethal`). enemy_hit_rate: the host's HITs a second per enemy (60 in all). The launcher writes none of these keys.
    bool enemy_hits = false;
    bool enemy_hits_set = false;
    int enemy_hit_owner = 0;
    int enemy_hit_path = 0;
    bool enemy_hits_lethal = false;
    int enemy_hit_rate = 30;
    // Enemy sync P1b (round 17, docs/enemy-sync.md 10; enemy_sync.hpp): ownership by proximity. An enemy near the guest and far
    // from the host is handed to her game (her copy unfrozen: it notices, targets and shoots at her Sam, takes her hits natively;
    // the host's copy becomes the frozen mirror) and goes back when she leaves, the host comes near, the link, the gate or a
    // world goes. enemy_owner (enemy_owner_set: the key was given): default off this round; both games need it (SEH1 parts
    // bit3). The launcher writes none.
    bool enemy_owner = false;
    bool enemy_owner_set = false;
    // P1c (round 21, docs/enemy-sync.md 13): enemy_owner hands over whole camps. enemy_owner_outcomes: her downs carried onto the
    // host's copy at a return (the outcome setter; 0: a camp with a member down on her side stays hers). enemy_follow_guard: the
    // host hides a frozen copy of hers that is unloaded or held there (no statues). enemy_attack_registry: MULE rounds with attack ids
    // above 0x18C (weapon 92's 0x191) replayed when this game's attack registry lists them (0: the old 0x18C cap).
    bool enemy_owner_outcomes = true;
    bool enemy_follow_guard = true;
    bool enemy_attack_registry = true;
    bool enemy_owner_hits = true;  // P1c: the host's hits on a frozen copy of one she owns relayed to her game (applied to hers)
    // Round 23 (docs/enemy-sync.md 14; cfgval::SetRestrainKey). enemy_restrain_relay: a tie-up (strand, bola) or knockout of a copy
    // of an enemy the other game owns is sent as HIT kind 5 and applied there with the outcome setter (k 2 restrained / k 0 faint)
    // instead of the old stun; both directions; both games need it (SEH1 parts bit 7). enemy_replay_flush: a shooter's queued shot
    // replays are dropped when its down outcome reaches this game. Both default on; the launcher writes neither.
    bool enemy_restrain_relay = true;
    bool enemy_replay_flush = true;
    // Round 23 B (docs/enemy-sync.md 14.8; cfgval::SetMirrorKey). enemy_mirror_hold_check: a mirror whose placement error is still
    // above 10 m after 3 teleports within 2 s is "not holding": given back, hidden and asleep here (never a statue), retried every
    // 5 s; its shots are still replayed from the owner's place. enemy_mirror_vehicle: the in-vehicle test (parent chain, else the
    // actor's ride fields +0xE20 / +0x3C0) every 1 s for every mirror and owned enemy, SEN1's kInVehicle bit, and a mirror whose
    // copy sits in a vehicle on either side given back to its own game. Both default on; the launcher writes neither.
    bool enemy_mirror_hold_check = true;
    bool enemy_mirror_vehicle = true;
    // Round 23 D (docs/enemy-sync.md 14.10; cfgval::SetTruckKey; vehicle_truck_rules.hpp), shared real vehicles only.
    // vehicle_mule_log: each truck in the vehicle pools is classified (a player truck; a MULE truck, empty or driven by a MULE
    // or a player) from its unit's rider (+0x158), its +0x19D bits and its entity's driver seat (+0x63C / +0x8B0); the driver,
    // seat and camp go to the snapshot / census logs and a change to "[vdrive] MULE truck ..." lines. vehicle_mule_hold: a
    // truck a non-player drives in either game, or any MULE truck no player drives, is left to each game: no PARK frames, no
    // parked teleport, no removal and no re-mirror by the shared vehicles. Both default on; the launcher writes neither.
    bool vehicle_mule_log = true;
    bool vehicle_mule_hold = true;
    // Round 23 E (docs/enemy-sync.md 14.11; cfgval::SetProbeKey). enemy_m2_probes: the debug action enemy_m2 (the M2 probes P1-P6:
    // the knowledge dump, the push levers, the perception reads, the raycast, the notice timing, the seated MULE's chain) is
    // allowed (the action itself stays hidden runs only), and her single takes get the read-only P2 window (D1-D8; armed by
    // itself at the take, round 26). On by default since round 26 (at the user's request); the launcher never writes it.
    bool enemy_m2_probes = true;
    // Round 25 (docs/enemy-sync.md 14.18; cfgval::SetTakeKey). The single-take levers, never written by the launcher, ON by default
    // since round 26 (docs/enemy-sync.md 14.19, at the user's request: none fixed the problems; K1 / F1 / C1 break stealth), each
    // off with =0: enemy_take_know (K1: her game forces a player visual of her Sam into a MULE she takes alone; 1 once, 2 then 2 Hz
    // for enemy_take_know_secs), enemy_take_camp (C1: her camp forced to 5 before the take; 1 when it was >= 4, 2 always),
    // enemy_ticket_return (T1: attack tickets held by her frozen copies returned), enemy_single_hold_vehicle (V1: the camp's
    // in-vehicle mirror release waits while a member of it is hers as a single take), enemy_single_rule (R1: the host's single takes follow RULE-W /
    // RULE-B). K1 / C1 / T1 write game state, in real games too since round 26, at a take of one MULE alone only.
    int enemy_take_know = 2;
    double enemy_take_know_secs = 3.0;
    int enemy_take_camp = 1;
    bool enemy_ticket_return = true;
    bool enemy_single_hold_vehicle = true;
    bool enemy_single_rule = true;
    // Round 25 e: enemy_take_role (R2: her taken MULE's camp role 1 reads 0 while it is hers), enemy_take_face (F1: her taken MULE
    // turned in place to face her Sam at the take). Both write game state; on by default since round 26 (real games too).
    bool enemy_take_role = true;
    bool enemy_take_face = true;
    // Round 27 M2 / M3 / M4 (docs/enemy-sync.md 16; cfgval::SetTargetKey, TargetDefaults), never written by the launcher. Round
    // 27 M4: on by default with enemy_hits (the destroy guard closed the body-destroy gap); a key given always wins (the *_set
    // flags). enemy_partner_stim (default 1 with enemy_hits): the partner body carries a visual stimulus made with our Sam's own
    // resource (enemy_target.cpp; the host only, only while the destroy guard is in), its visibility from the partner's SPV1
    // (enemy_partner_vis), only while a realized human enemy is within enemy_partner_stim_radius m.
    // enemy_target_pick (default 71 = 1 + 2 + 4 + 64 with enemy_hits; bits: 1 reorder, 2 MULE-only perceivers, 4 classifier flag
    // fix, 8 her hits force a visual, 16 line-of-sight keep, 32 her record from ours (needs 1 and 16), 64 take first),
    // enemy_target_redirect (default 35 = 1 + 2 + 32 with enemy_hits; bits: 1 CanShoot's point, 2 turn / range check, 4 approach,
    // 16 vehicle chase, 32 shoot actor; 8 deferred),
    // enemy_target_fresh_ms (500..3000).
    // The int values below are the "not given" placeholders: config.cpp applies TargetDefaults after the parse.
    int enemy_partner_stim = 0;
    bool enemy_partner_vis = true;
    double enemy_partner_stim_radius = 300.0;
    int enemy_target_pick = 0;
    int enemy_target_redirect = 0;
    int enemy_target_fresh_ms = 1500;
    bool enemy_partner_stim_set = false, enemy_target_pick_set = false, enemy_target_redirect_set = false;
    // Round 23 C (docs/enemy-sync.md 14.9; cfgval::SetDownKey, DownRadius). enemy_down_hold: while the other player is down (death or
    // KnotSpace, from its SEH1 beacon; never a faint), the copies of that game's fights here are held frozen in their last pose (or
    // hidden if never posed; a down one far away hidden), never given back alive, no new mirror, no ownership change, until that
    // player is back or the world / link resets. enemy_down_hold_radius: m from our player for "near". Default on, 120 m; the
    // launcher writes neither.
    bool enemy_down_hold = true;
    double enemy_down_hold_radius = 120.0;
    // The partner's vehicle copy hidden while the watcher's own vehicle of the same kind stands in the same place (two of
    // the same vehicle in one spot; docs/real-partner-body.md section 22). Default on; 0: the copy is always shown.
    bool vehicle_overlap_hide = true;
    // vehicle_look: the partner's vehicle copy wears the partner vehicle's paint and variations (gold / silver / broken /
    // ride type), its wear (rust), lamps and battery gauge (the solid trike and truck copies only; docs/real-partner-body.md
    // sections 22.7, 22.12) and carries its cargo as case props (every copy, the holograms too). Default on; 0: none of it
    // is read, sent or applied (the copies' own look, no cargo). vehicle_packs: its battery / defense packs (solid copies
    // only; default on; 0: not read, sent or shown).
    bool vehicle_look = true;
    bool vehicle_packs = true;
    bool pose_hook = false;           // install the pose hook even without partner_body=real (debug spawns)

    // Debug
    bool verbose = false;
    bool debug_menu = true;            // in-game ImGui debug menu (D3D12 overlay)
    std::uint32_t debug_menu_vk = 0x78;  // virtual-key that toggles it (default VK_F9)
    // dev_tools=1: the debug menu's Dev tools page and the dev_* debug actions (dev_tools.hpp; the launcher's Debug page
    // "Dev tools"). Tools that change what a save keeps run only on a sandboxed save (dev_rules.hpp). Hidden runs have them on.
    bool dev_tools = false;
    // dev_give=0|1 (default 1; round 22): the Dev tools page's Give buttons and dev_give (dev_tools.cpp, give_rules.hpp): the game's
    // own CreateAndAddBaggageToPlayer gives Sam weapons, tools and equipment (sandboxed saves only, like dev_items).
    // dev_give_max_kg (1..200, default 30): the weight one press may add; dev_give_per_frame (1..16, default 4): requests a tick.
    bool dev_give = true;
    double dev_give_max_kg = 30.0;
    int dev_give_per_frame = 4;
    // dev_give_slot (0..31, default 1: Sam's own inventory): the u8 passed to CreateAndAddBaggageToPlayer (give_rules.hpp Slot:
    // the destination kind; 0 leaves the baggages unheld at the world origin).
    int dev_give_slot = 1;
    bool one_shot = false;           // consumed on load (renamed to session.last.ini)
    // name=<text>: the name shown above this player's head on the partner's screen (name tags);
    // "" = the Steam persona name. Sent over the link (STN1), never logged in full.
    std::string player_name;
    // name_tags=0|1 (default 1): show the partner's name above their head (overlay) and send our
    // own (STN1). 0: neither (the partner then shows "Host" / "Guest" for us).
    bool name_tags = true;
    // partner_collision=0|1 (default 1): the local player cannot walk through the partner's real body
    // (the game's own character capsule on that body; docs/real-partner-body.md section 11).
    // partner_collision_layer=10|9|25 (default 10 "Humanoid blocker"; 9 = the game's "AI or Remote
    // Player" layer, which bullets and cargo hit too; 25 "vs Humanoids" did not block reliably). Test setting.
    // partner_collision_height=<m> (default 4.0; 0 = Sam's own 1.65 m): the capsule's standing height. Taller
    // than Sam can climb or reach with a jump-grab (2.6 m was still grabbed), so the partner cannot be climbed
    // (section 11.5).
    bool partner_collision = true;
    int partner_collision_layer = 10;
    float partner_collision_height = 4.0f;
    // name_tag_font=game|default (default game): name tags use the game's own menu font ("EX PS Medium
    // Neon", the "Map/Information" header), read at run time from the installed game's archives into
    // memory only (never written or shipped); default = the overlay's Segoe UI.
    bool name_tag_game_font = true;
    // partner_gear=0|1 (default 1): the partner's real body (and the self view's body) carries the
    // player's backpack, Odradek, BB pod and cargo (docs/real-partner-body.md section 12), and our
    // own gear layout is sent (STC1). 0: nothing is read, sent or spawned.
    bool partner_gear = true;
    // smooth_pose=0|1 (default 1; docs/real-partner-body.md section 3.5): the partner's and the self
    // view's bodies (and their gear props) get a fresh pose every presented frame above 60 fps: 240 Hz
    // model updates, interpolation inside the body's own update, capture-time stamps on STJ1. 0 = the
    // v0.7 behaviour (bodies updated at the game's 59.94 Hz for cutscene actors).
    bool smooth_pose = true;
    // audio_probe=0|1 (default 0; docs/audio-research.md section 9, Phase 0): logging-only probes on Sam's voice path
    // (voice queue, talk handler, animation sound events) and on the exported Wwise API (PostEvent, RegisterGameObj,
    // SetSwitch, SetRTPCValue, SetPosition, StopPlayingID per game object). Changes nothing the game does.
    bool audio_probe = false;
    // self_voice=auto|sam|mute (docs/audio-research.md section 9, Phase 1; self_voice.cpp): Sam's own voice (spoken
    // lines, grunts, efforts, breathing) on THIS player's game. auto = muted when this player's character
    // (my_character; the host is always sam) is not Sam, and the cargo menu's figure while it shows that character;
    // sam = as the game plays it; mute = always muted. Absent (or not understood) = sam: nothing is hooked. The
    // launcher writes auto (Debug option, default on) or sam.
    selfvoice::Mode self_voice = selfvoice::Mode::sam;
    // voice_sync=0|1 (default 0; docs/audio-research.md section 15, Phase 2; partner_voice.cpp): each player hears
    // the other's voice. The local player's talks (Sam's lines, grunts, efforts, breathing) go out as STS1 on the
    // link's reliable channel (a player who is not Sam sends only "muted" frames: never Sam's lines), and the
    // partner's are played with the game's DSTalkComponent::PlaySentence on an invisible talking entity (the echo
    // resource, as partner_voice=1 spawns it) kept at the partner body's head. 0: nothing is hooked, sent or played
    // for it (self_voice still mutes).
    bool voice_sync = false;
    // voice_subtitles=0|1 (default 0): with voice_sync, the game may show the partner's lines as subtitles; 0 drops
    // the subtitle of a line the partner voice just played (the subtitle hook, byte-checked), 1 leaves them.
    bool voice_subtitles = false;
    // sound_sync=0|1 (default 0; docs/audio-research.md section 16, Phase 3; partner_sound.cpp): each player hears the
    // other's footsteps, foley, cargo and weapon sounds. The Wwise events posted on the local player's own game objects
    // (the SoundComponents of the player and his child entities: animation foley, weapons, gear, cargo) go out as STA1 in
    // the pose datagram, with their RTPC values; the partner's are posted on game objects of our own kept at the partner
    // body's joints, at the pose play time. 0: nothing is hooked, sent or posted for it.
    bool sound_sync = false;
    // sound_steps=0|1 (default 1; with sound_sync): footsteps and the movement layer, which the game plays from material
    // effect emitters with no owner, are taken as the local player's by position (within 1 m of his root or 0.5 m of a
    // foot; experimental: another walker that close is mistaken for him). 0: only the sounds tied to the player exactly.
    bool sound_steps = true;
    // partner_volume=0..100 (default 100; with sound_sync; Phase 4): the partner's footsteps, foley and weapon sounds at this
    // percent of their normal volume (SetGameObjectOutputBusVolume on the receiver's own Wwise objects); 0 posts nothing.
    // The partner's voice is not affected. Out of range: clamped.
    int partner_volume = 100;
    // vehicle_sound=0|1 (default 1; with sound_sync and vehicle_sync; docs/real-partner-body.md section 22.9): the engine,
    // road noise and other sounds of the vehicle the partner rides (its own SoundComponent's Wwise events, with their
    // RTPCs: rpm, speed, throttle, boost...) are posted at the partner's vehicle copy, at partner_volume. 0: only the
    // player's own sounds are sent and posted (the v0.12 behaviour).
    bool vehicle_sound = true;


    // Local two-instance test (docs/sandbox-and-local-test.md): local_input=host|host_pad|guest.
    // Read here for the overlay's input split; the sandbox keys (sandbox_saves, sandbox_foreign_saves,
    // appdir_settings, second_instance) are read by automation.cpp in DllMain.
    std::string local_input;           // "" = a normal game
    std::string sandbox_saves;         // the test-save sandbox this game runs on ("" = the real saves)
    // host_save=1 (docs/coop-session.md, M1): the host-held save session. Host: plays on its own saves and offers them to
    // the guest (read only). Guest: never has a save of its own; DllMain refuses it without a confirmed save redirect
    // (sandbox.hpp Mode::refuse_host_save) and it loads the host's save the link brings. 0 / absent: each player on their
    // own save (the old mode).
    bool host_save = false;
    // coop_test_build=<text> (hidden tests only, the version scenario): this game reports that game build instead of its
    // own in the co-op save session's version check. Ignored outside a hidden run (automation=1).
    std::string coop_test_build;
    // coop_test_title_off=1 (hidden tests only): the guest's title in the game's own menu is not installed, as if its byte
    // checks failed: the full-screen waiting panel fallback with its one-line reason. Ignored outside a hidden run.
    bool coop_test_title_off = false;
    // guest_skip_logos (default 1): the guest of a host-held save session skips the game's four boot logos by the splash
    // page's own skip-all path (title_wait.cpp), so its waiting title is up ~12 s sooner. 0: the logos play as always.
    bool guest_skip_logos = true;
    unsigned frame_cap = 0;            // frame_cap=<15..240>: at most this many frames per second (0 = off)
    std::string local_window;          // local_window=left|right: the game window takes that half of the monitor
    // mouse_lock=1 (the launcher's Local test, Debug page "Lock the mouse to the focused game window"): the cursor stays
    // inside the client area of the game window that is in front; F10 (F11 when the menu key is F10) switches to the
    // other game's window, Alt+Tab leaves (overlay/mouse_lock.hpp). Absent / 0: the game's own cursor handling only.
    bool mouse_lock = false;
    // test_net_latency_ms / test_net_jitter_ms / test_net_loss_permille / test_net_burst (hidden tests only, the UDP link):
    // this game's datagrams leave late (latency + uniform jitter) and some are lost (per mille; burst > 0: Gilbert-Elliott
    // bursts of that mean length), through the transport's lab impairment (udp_transport.hpp). Both games set: the round
    // trip is twice the latency plus the jitter. Ignored outside a hidden run (automation=1) and on the Steam transport.
    unsigned test_net_latency_ms = 0, test_net_jitter_ms = 0, test_net_loss_permille = 0, test_net_burst = 0;
};

// Loads <dir>\session.ini. Never throws; unknown keys are logged and ignored.
Config LoadConfig(const std::wstring& dir);
// dllmain.cpp: the frame cap at run time (session.ini frame_cap sets it at start; the `smooth` test
// action changes it). 0 = off, else clamped to 15..240.
void SetRuntimeFrameCap(unsigned fps) noexcept;
unsigned RuntimeFrameCap() noexcept;
const char* RoleName(Role r);
const char* PartnerBodyName(PartnerBody b);

}  // namespace st
