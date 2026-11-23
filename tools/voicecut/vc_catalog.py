"""voicecut: the catalog of Fragile's and Sam's voice lines, and the categories clips are cut into. Python 3 stdlib.

Catalog items (one per playable source):
  line      a sentence with its own audio (Fragile: terminal and mission radio lines; Sam: his 2,000 player lines)
  cutscene  a cutscene's dialogue track (every voice in that cutscene, no music) for each cutscene where Fragile
            speaks: her grunts and breaths exist only there, so these are what you cut her efforts from

Sam's lines carry the situations the game plays them in (ds_player_sentence_resident). Only some situation names are
known (the game keeps 31-bit CRC32-C hashes); a known name gives a category guess, the rest are listed by hash and
can be put in a category in the UI (user state, not this file).
"""
import json
import os
import time

import vc_game as G

CATALOG_VERSION = 1

# (key, label, what Sam's versions sound like)
CATEGORIES = [
    ('stumble', 'Stumble / slip', 'tripping, slipping, losing balance'),
    ('fall', 'Fall', 'falling over, tumbling down a slope'),
    ('landing', 'Landing', 'landing after a jump or a drop'),
    ('jump', 'Jump', 'the push of a jump'),
    ('heavy_lift', 'Heavy lift', 'picking up, carrying and holding heavy cargo'),
    ('effort', 'Effort', 'short exertion grunts: climbing, pushing, kicking'),
    ('breathing', 'Breathing', 'out of breath, panting, tired breathing'),
    ('pain', 'Pain', 'being hit, hurt or choked'),
    ('climb', 'Climb / rope', 'steep slopes, ropes, ladders'),
    ('cold_water', 'Cold / water', 'cold, wading, drowning'),
    ('hum', 'Humming', 'humming and singing'),
    ('rest', 'Rest / sleep', 'sitting down, snoring, stretching'),
    ('reaction', 'Reaction', 'surprise, relief, gestures'),
    ('random_line', 'Random line', 'spoken remarks'),
    ('other', 'Other', 'anything else'),
]
CATEGORY_KEYS = [c[0] for c in CATEGORIES]

# Situation names the game's code or data spell out (docs/audio-research.md 3.1), and the category each suggests.
KNOWN_SITUATIONS = {
    'Jumping': 'jump', 'JumpingDouble': 'jump', 'LandingFromJumpHigher': 'landing', 'SlipByRain': 'stumble',
    'OutOfBreath': 'breathing', 'IncreasinglyExhaustedBreathing': 'breathing',
    'MovingBreathingWhenOverburdened': 'breathing', 'IdleBreathingWhenOverburdened': 'breathing',
    'BreathingWhenClimbingSteepSlope': 'breathing', 'BreathingWhenWadingThroughWater': 'breathing',
    'HoldBaggageNormal': 'heavy_lift', 'HoldBaggageFast': 'heavy_lift', 'HoldOnBig': 'heavy_lift',
    'SteepSlope': 'climb', 'UseRopeStart': 'climb', 'UseRopeEnd': 'climb',
    'StrongDamageBatting': 'pain', 'WeakDamageBatting': 'pain', 'WeakDamageBattingOrz': 'pain', 'Faint': 'pain',
    'CartImpact': 'pain', 'HiggsChoking': 'pain', 'HiggsChokingLoop': 'pain', 'PrivateChoke': 'pain',
    'StrongPunchKick': 'effort', 'PushBackHiggsAfterEarAttack': 'effort', 'ShookOffHunter': 'effort',
    'Drowning': 'cold_water', 'DrowningEnd': 'cold_water', 'Cold': 'cold_water',
    'SleepColdPlace': 'rest', 'Snore': 'rest', 'MassagingShoulder': 'rest', 'PrivateNeckStretch': 'rest',
    'PrivateLeanBack': 'rest',
    'Humming': 'hum', 'HummingToComfortBB': 'hum',
    'NearlyCaughtByHunters': 'reaction', 'LookDown': 'reaction', 'LookingAtBrokenShoes': 'reaction',
    'PrivateFistPumps': 'reaction', 'PrivateLeanPoint': 'reaction', 'PrivateLookAway': 'reaction',
    'PrivateMiddleFinger': 'reaction', 'PrivateRockOn': 'reaction', 'PrivateWink': 'reaction',
    'ShoelessWalk': 'reaction', 'LeaveHiggs': 'reaction',
    'ReceivedLike': 'random_line', 'AlmostThere': 'random_line', 'OnVehicle': 'random_line',
    'StartRidingCart': 'random_line',
}
# Hashes seen at run time from Sam's animation voice events (audio-research.md 12.1): labels only.
ANIMATION_VOICE = {0x35FEDCC7: 'animation voice 2500', 0x341E4861: 'animation voice 2501 (walk/idle efforts)',
                   0x39C69516: 'animation voice 2502', 0x21A05718: 'animation voice 2509',
                   0x1EB43780: 'animation voice 2510', 0x3F8A5E65: 'animation voice 2520'}
ANIMATION_GUESS = {0x341E4861: 'effort'}

NAME_BY_HASH = {G.situation_hash(n): n for n in KNOWN_SITUATIONS}


def situation_label(h):
    return NAME_BY_HASH.get(h) or ANIMATION_VOICE.get(h) or 'situation %08X' % h


def guess_situation(h, spoken, nonverbal, prio=None):
    """(category, sure): a known name's category, else a guess from the lines (spoken -> random line)."""
    name = NAME_BY_HASH.get(h)
    if name:
        return KNOWN_SITUATIONS[name], True
    if h in ANIMATION_GUESS:
        return ANIMATION_GUESS[h], False
    if spoken and not nonverbal:
        return 'random_line', False
    return 'other', False


def line_id(speaker, guid):
    return '%s-%s' % (speaker[0], guid)


def build_catalog(gd, language='english', progress=None):
    """Scan the game: every Fragile and Sam line with audio, Sam's situations, Fragile's cutscene dialogue tracks."""
    t0 = time.time()

    def say(msg, done=None, total=None):
        if progress:
            progress(msg, done, total)

    say('Indexing the game archives (first time only)')
    gd.index()
    say('Listing the game files')
    cores = G.sentence_cores(gd.all_paths())
    items = []
    frg_cutscenes = {}
    for k, path in enumerate(cores):
        if k % 10 == 0:
            say('Reading sentence files', k, len(cores))
        try:
            core = gd.read(path)
        except KeyError:
            continue
        cut = G.cutscene_of(path)
        for ln in G.scan_sentence_core(core, path, language):
            if cut and ln['speaker'] == 'fragile' and ln['text']:
                frg_cutscenes.setdefault(cut, []).append(ln['text'])
            if not ln['stream']:
                continue
            src = path.split('/')[2] if path.count('/') >= 3 else path
            items.append({'id': line_id(ln['speaker'], ln['guid']), 'kind': 'line', 'speaker': ln['speaker'],
                          'text': ln['text'], 'length': ln['length'], 'stream': ln['stream'], 'core': path,
                          'group': src.replace('ds_lines_', ''), 'situations': [], 'category': None, 'sure': False})
    say('Reading Sam\'s situations')
    by_sentence, prio, _flags, all_hashes = G.scan_sam_situations(gd.read(G.SAM_RESIDENT), gd.read(G.SAM_PRIORITY))
    sit_lines = {}
    for it in items:
        if it['speaker'] != 'sam':
            continue
        guid = it['id'][2:]
        it['situations'] = by_sentence.get(guid, [])
        for h in it['situations']:
            sit_lines.setdefault(h, []).append(it)
    situations = []
    for h in all_hashes:
        lines = sit_lines.get(h, [])
        spoken = sum(1 for i in lines if i['text'])
        cat, sure = guess_situation(h, spoken, len(lines) - spoken)
        p = prio.get(h) or {}
        situations.append({'hash': h, 'label': situation_label(h), 'known': h in NAME_BY_HASH, 'guess': cat,
                           'sure': sure, 'lines': len(lines), 'spoken': spoken, 'priority': p.get('priority'),
                           'reaction': p.get('reaction', False), 'dialogue': p.get('dialogue', False)})
    sit_by_hash = {s['hash']: s for s in situations}
    for it in items:
        if it['speaker'] == 'sam':
            cats = [sit_by_hash[h]['guess'] for h in it['situations'] if h in sit_by_hash]
            it['category'] = cats[0] if cats else ('random_line' if it['text'] else 'other')
            it['sure'] = any(sit_by_hash[h]['sure'] for h in it['situations'] if h in sit_by_hash)
        else:
            it['category'] = 'random_line' if it['text'] else 'other'
    say('Finding Fragile\'s cutscenes')
    for (cs, seq), texts in sorted(frg_cutscenes.items()):
        sp = G.CINEMATIC_SOUND.format(cs=cs, seq=seq)
        try:
            core = gd.read(sp)
        except KeyError:
            continue
        for tid, body in G.read_objects(core):
            if tid != G.TYPE_WEM_LOCALIZED:
                continue
            w = G.parse_wem_localized(body)
            stream = w['streams'].get(language) or w['streams'].get('english')
            if stream:
                items.append({'id': 'c-' + seq, 'kind': 'cutscene', 'speaker': 'fragile', 'group': 'cutscene',
                              'text': ' / '.join(texts), 'lines': len(texts), 'length': 0.0, 'stream': stream,
                              'core': sp, 'situations': [], 'category': None, 'sure': False, 'name': seq})
            break
    say('Done', 1, 1)
    return {'version': CATALOG_VERSION, 'language': language, 'game': G.game_stamp(gd.game_dir),
            'built': int(time.time()), 'seconds': round(time.time() - t0, 1), 'items': items,
            'situations': situations, 'categories': [{'key': k, 'label': l, 'hint': h} for k, l, h in CATEGORIES]}


def load_catalog(path, game_stamp, language):
    try:
        with open(path, encoding='utf-8') as f:
            c = json.load(f)
    except (OSError, ValueError):
        return None
    if c.get('version') != CATALOG_VERSION or c.get('game') != game_stamp or c.get('language') != language:
        return None
    return c


def save_catalog(path, cat):
    if os.path.dirname(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(cat, f, ensure_ascii=False, separators=(',', ':'))
    os.replace(tmp, path)
