"""voicecut: the voice data of Death Stranding Director's Cut, read from the installed game. Python 3 stdlib only.

The object layouts below are hard-coded from the serialized field order (no type JSON is needed or shipped), like
native/textures does for textures; docs/voicecut.md has the details and how each was checked.

  SentenceResource          one line: Delivery, ShowSubtitles, SimpleSound (the audio), Text (subtitles), Voice
                            (who speaks: localized/voices/vr0040_frg = Fragile, vr0010_sam = Sam)
  LocalizedSimpleSoundResource  per-language lengths and, after its fields, one stream path per language:
                            "<sentences core>_sentence_<guid>.wem.<language>"; the audio is "<that>.core.stream", a
                            Wwise Vorbis WEM (vc_vorbis)
  LocalizedTextResource     per language: u16 length, UTF-8 text, u16 length, notes, u8 flags (English first)
  WwiseWemLocalizedResource a cutscene's dialogue track: "..._voice_track.<language>" (all voices, no music)
  DSSentenceSituationResource / DSPlayerSentenceResource / DSSentenceSituationPriorityInfoResource
                            Sam's situations (ds/entities/player/sentence/*): which of his lines the game plays for
                            which situation hash (CRC32-C of the name, 31 bits)

Archive access is tools/re/ds_archive.py (read-only; the game's own oo2core decompresses).
"""
import os
import re
import struct
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
RE_TOOLS = os.path.join(HERE, '..', 're')

TYPE_SENTENCE = 0xAD7F486B5DD745A4
TYPE_SIMPLE_SOUND = 0x859E0AC074117955
TYPE_TEXT = 0x31BE502435317445
TYPE_WEM_LOCALIZED = 0x3DE345CD4B0C7E72
TYPE_SITUATION = 0x50AC2E322EDCCA3B
TYPE_PLAYER_SENTENCE = 0xDB061C932BA5209D
TYPE_PRIORITY_INFO = 0x261040389F23D98B

# LocalizedTextResource language order (ELanguage 1..25); the stream suffixes use the lower-case names.
TEXT_LANGUAGES = ['english', 'french', 'spanish', 'german', 'italian', 'dutch', 'portuguese', 'chinese_traditional',
                  'korean', 'russian', 'polish', 'danish', 'finnish', 'norwegian', 'swedish', 'japanese', 'latamsp',
                  'latampor', 'turkish', 'arabic', 'chinese_simplified', 'english_uk', 'greek', 'czech', 'hungarian']
# LengthInSeconds / WemIDs index of a language (index 0 is unused)
def language_index(lang):
    return TEXT_LANGUAGES.index(lang) + 1 if lang in TEXT_LANGUAGES else 1


SPEAKERS = {'fragile': 'vr0040_frg', 'sam': 'vr0010_sam'}
SAM_RESIDENT = 'ds/entities/player/sentence/ds_player_sentence_resident'
SAM_PRIORITY = 'ds/entities/player/sentence/ds_player_sentence_priority'
CINEMATIC_SOUND = 'ds/sounds/wwise_cinematics_sound_resource/{cs}/{seq}/{seq}_sound'
PREFETCH = 'prefetch/fullgame.prefetch'


class FormatError(ValueError):
    pass


def read_objects(core):
    """[(type_id, body)] of a core file; FormatError when the objects do not end exactly at its end."""
    out = []
    pos, n = 0, len(core)
    while pos + 12 <= n:
        tid, size = struct.unpack_from('<QI', core, pos)
        if pos + 12 + size > n:
            raise FormatError('object at %d overruns the core' % pos)
        out.append((tid, core[pos + 12:pos + 12 + size]))
        pos += 12 + size
    if pos != n:
        raise FormatError('core parse ended at %d of %d' % (pos, n))
    return out


def guid_text(b):
    return str(uuid.UUID(bytes_le=bytes(b[:16])))


def read_ref(b, p):
    """Ref<...>: u8 kind; a GUID unless kind 0; then a path string (u32 length, u32 crc, bytes) when kind >= 2.
    Returns ((kind, guid, path) or None, new position)."""
    kind = b[p]
    p += 1
    if kind == 0:
        return None, p
    g = guid_text(b[p:p + 16])
    p += 16
    path = ''
    if kind >= 2:
        ln = struct.unpack_from('<I', b, p)[0]
        p += 4
        if ln:
            path = bytes(b[p + 4:p + 4 + ln]).decode('utf-8', 'replace')
            p += 4 + ln
    return (kind, g, path), p


def parse_sentence(body):
    """SentenceResource: GUID, Delivery u8, PostDelay f32, ShowSubtitles bool, SimpleSound, Animation, Text, Voice."""
    p = 16
    delivery = body[p]
    p += 1 + 4
    subs = bool(body[p])
    p += 1
    sound, p = read_ref(body, p)
    _anim, p = read_ref(body, p)
    text, p = read_ref(body, p)
    voice, p = read_ref(body, p)
    if p != len(body):
        raise FormatError('SentenceResource is %d bytes, read %d' % (len(body), p))
    return {'guid': guid_text(body), 'delivery': delivery, 'subtitles': subs,
            'sound': sound[1] if sound else None, 'text': text[1] if text else None,
            'voice': voice[2] if voice else ''}


def find_stream_paths(body, start=0):
    """{language: stream path} from the custom data of a localized sound: every length-prefixed path that ends in
    '.<language>'."""
    out = {}
    data = bytes(body)
    for lang in TEXT_LANGUAGES:
        needle = b'.' + lang.encode()
        i = data.find(needle, start)
        while i >= 0 and lang not in out:
            end = i + len(needle)   # the bytes after the name are binary and may be letters: the prefix decides
            for s in range(i - 1, max(start + 4, end - 600) - 1, -1):
                if struct.unpack_from('<I', data, s - 4)[0] == end - s:
                    path = data[s:end]
                    if re.fullmatch(rb'[\x21-\x7e]+', path):
                        out[lang] = path.decode('ascii')
                    break
            i = data.find(needle, i + 1)
    return out


def parse_simple_sound(body):
    """LocalizedSimpleSoundResource: GUID, NameHash, Group ref, 132 fixed bytes (volumes, shape, attenuation...),
    ExternalSourceCookie u32, Flags u8, SoundMixState ref, Preset ref, LengthInSeconds Array<f32>, WemIDs
    Array<u32>, VolumeCorrectionRTPCValue f32, then the per-language stream table."""
    p = 16 + 4
    _group, p = read_ref(body, p)
    p += 132
    cookie = struct.unpack_from('<I', body, p)[0]
    p += 4 + 1
    _mix, p = read_ref(body, p)
    _preset, p = read_ref(body, p)
    n = struct.unpack_from('<I', body, p)[0]
    if n > 64:
        raise FormatError('LocalizedSimpleSoundResource: %d lengths' % n)
    lengths = list(struct.unpack_from('<%df' % n, body, p + 4))
    p += 4 + 4 * n
    m = struct.unpack_from('<I', body, p)[0]
    if m > 64:
        raise FormatError('LocalizedSimpleSoundResource: %d wem ids' % m)
    wems = list(struct.unpack_from('<%dI' % m, body, p + 4))
    p += 4 + 4 * m + 4
    return {'guid': guid_text(body), 'cookie': cookie, 'lengths': lengths, 'wems': wems,
            'streams': find_stream_paths(body, p)}


def parse_text(body):
    """LocalizedTextResource: GUID, then per language u16 length + UTF-8, u16 length + notes, u8 flags."""
    p = 16
    texts = []
    while p + 2 <= len(body) and len(texts) < len(TEXT_LANGUAGES):
        ln = struct.unpack_from('<H', body, p)[0]
        t = bytes(body[p + 2:p + 2 + ln]).decode('utf-8', 'replace')
        p += 2 + ln
        if p + 2 > len(body):
            texts.append(t)
            break
        nl = struct.unpack_from('<H', body, p)[0]
        p += 2 + nl + 1
        texts.append(t)
    return {'guid': guid_text(body), 'texts': texts}


def parse_wem_localized(body):
    """WwiseWemLocalizedResource: IsStreaming bool, GUID, WemID, WemSize, WemData Array<u8>, BitField u16, then
    custom data with the per-language stream table (and the length)."""
    p = 1
    g = guid_text(body[p:p + 16])
    p += 16
    wem_id, wem_size = struct.unpack_from('<II', body, p)
    p += 8
    n = struct.unpack_from('<I', body, p)[0]
    p += 4 + n + 2
    return {'guid': g, 'wem_id': wem_id, 'size': wem_size, 'streams': find_stream_paths(body, p)}


def parse_situation(body):
    """DSSentenceSituationResource: GUID, ConditionalSentenceResources Array<Ref>, SituationStringHash u32."""
    p = 16
    n = struct.unpack_from('<I', body, p)[0]
    p += 4
    refs = []
    for _ in range(n):
        r, p = read_ref(body, p)
        if r:
            refs.append(r[1])
    h = struct.unpack_from('<I', body, p)[0]
    return {'guid': guid_text(body), 'players': refs, 'hash': h}


def parse_player_sentence(body):
    """DSPlayerSentenceResource: GUID, SentenceResource ref, NoSpeakFlag u64, EchoSentenceIndex i32, IsNeedWaterVoice,
    IsTiredVoice, IsLongSentence."""
    r, p = read_ref(body, 16)
    _nospeak, _echo = struct.unpack_from('<Qi', body, p)
    p += 12
    water, tired, long_ = body[p], body[p + 1], body[p + 2]
    return {'guid': guid_text(body), 'sentence': r[1] if r else None, 'path': r[2] if r else '',
            'water': bool(water), 'tired': bool(tired), 'long': bool(long_)}


def parse_priority(body):
    """DSSentenceSituationPriorityInfoResource: GUID, SituationHash, Priority, Flag, ..., IsDialogue,
    IsNeedContextCheckDialogue, IsReactionVoice (the last three bytes)."""
    h, prio = struct.unpack_from('<Ii', body, 16)
    return {'hash': h, 'priority': prio, 'dialogue': bool(body[-3]), 'reaction': bool(body[-1])}


# --------------------------------------------------------------- CRC32-C situation hash

_CRC = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if _c & 1 else _c >> 1
    _CRC.append(_c)


def situation_hash(name):
    """The engine's name hash: CRC32-C, initial value 0, no final xor, masked to 31 bits."""
    c = 0
    for ch in name.encode('utf-8'):
        c = _CRC[(c ^ ch) & 0xFF] ^ (c >> 8)
    return c & 0x7FFFFFFF


# --------------------------------------------------------------- the game

DEFAULT_GAME = r'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT'


def find_game_dir(explicit=None):
    """The installed game: an explicit folder, ST_GAME_DIR, the default Steam path, or another Steam library listed in
    libraryfolders.vdf."""
    cands = [explicit, os.environ.get('ST_GAME_DIR'), DEFAULT_GAME]
    for env in ('ProgramFiles(x86)', 'ProgramFiles'):
        base = os.environ.get(env)
        if base:
            vdf = os.path.join(base, 'Steam', 'steamapps', 'libraryfolders.vdf')
            try:
                with open(vdf, encoding='utf-8', errors='replace') as f:
                    for m in re.finditer(r'"path"\s+"([^"]+)"', f.read()):
                        cands.append(os.path.join(m.group(1).replace('\\\\', '\\'), 'steamapps', 'common',
                                                  'DEATH STRANDING DIRECTORS CUT'))
            except OSError:
                pass
    for c in cands:
        if c and os.path.isfile(os.path.join(c, 'ds.exe')) and os.path.isdir(os.path.join(c, 'data')):
            return os.path.abspath(c)
    return None


def game_stamp(game_dir):
    st = os.stat(os.path.join(game_dir, 'ds.exe'))
    return '%d-%d' % (st.st_size, int(st.st_mtime))


class GameData:
    """Read-only access to the game's packfiles through tools/re/ds_archive.py. Nothing is written to the game
    folder: the archive index cache goes to `cache_dir`."""

    def __init__(self, game_dir, cache_dir):
        if RE_TOOLS not in sys.path:
            sys.path.insert(0, os.path.abspath(RE_TOOLS))
        import ds_archive  # noqa: E402  (tools/re: the repo's archive reader)
        self.A = ds_archive
        g = os.path.normcase(os.path.abspath(game_dir))
        c = os.path.normcase(os.path.abspath(cache_dir))
        if c == g or c.startswith(g + os.sep):
            raise ValueError('refusing to keep the cache inside the game folder')
        os.makedirs(cache_dir, exist_ok=True)
        self.game = ds_archive.Game(game_dir, cache_dir)
        self.game_dir = game_dir

    def index(self):
        return self.game.index()

    def has(self, path):
        return bool(self.game.locate(path)[1])

    def read(self, path):
        return self.game.read(path)

    def stream(self, path):
        """A '.core.stream' file: path is the stream table's path (without '.core.stream')."""
        return self.game.read(path + '.core.stream')

    def all_paths(self):
        data = self.read(PREFETCH)
        try:
            files = self.A.parse_prefetch(data)
        except Exception as e:
            raise FormatError('prefetch list: %s' % e)
        if isinstance(files, tuple):
            files = files[0]
        return [f[:-5] if f.endswith('.core') else f for f in files]


def sentence_cores(paths):
    return sorted({p for p in paths if p.startswith('localized/sentences/') and p.endswith('/sentences')})


def cutscene_of(core_path):
    """'localized/sentences/ds_lines_cutscene/sq_cs05_s00916/sentences' -> ('cs05', 'sq_cs05_s00916')"""
    m = re.match(r'localized/sentences/ds_lines_cutscene/(sq_(cs\d+)_s\d+\w*)/sentences$', core_path)
    return (m.group(2), m.group(1)) if m else None


def scan_sentence_core(core, path, language='english', speakers=SPEAKERS):
    """Lines of the wanted speakers in one sentences core: dicts with speaker, guid, text, stream, length."""
    objs = read_objects(core)
    sounds, texts, sents = {}, {}, []
    want = {v: k for k, v in speakers.items()}
    for tid, body in objs:
        if tid == TYPE_SENTENCE:
            s = parse_sentence(body)
            voice = s['voice'].rsplit('/', 1)[-1]
            if voice in want:
                s['speaker'] = want[voice]
                sents.append(s)
    if not sents:
        return []
    li = language_index(language)
    ti = TEXT_LANGUAGES.index(language) if language in TEXT_LANGUAGES else 0
    need_s = {s['sound'] for s in sents if s['sound']}
    need_t = {s['text'] for s in sents if s['text']}
    for tid, body in objs:
        if tid == TYPE_SIMPLE_SOUND and guid_text(body) in need_s:
            sounds[guid_text(body)] = parse_simple_sound(body)
        elif tid == TYPE_TEXT and guid_text(body) in need_t:
            texts[guid_text(body)] = parse_text(body)
    out = []
    for s in sents:
        snd = sounds.get(s['sound'])
        t = texts.get(s['text'])
        text = ''
        if t and t['texts']:
            text = t['texts'][ti] if ti < len(t['texts']) and t['texts'][ti] else t['texts'][0]
        line = {'speaker': s['speaker'], 'guid': s['guid'], 'core': path, 'text': text,
                'delivery': s['delivery'], 'stream': None, 'length': 0.0}
        if snd:
            line['stream'] = snd['streams'].get(language) or snd['streams'].get('english')
            if li < len(snd['lengths']):
                line['length'] = round(snd['lengths'][li] or max(snd['lengths'] or [0]), 3)
        out.append(line)
    return out


def scan_sam_situations(resident_core, priority_core=None):
    """{sentence guid: [situation hash...]}, {hash: priority info}, {hash: [player sentence flags]}"""
    players, by_sentence = {}, {}
    sits = []
    for tid, body in read_objects(resident_core):
        if tid == TYPE_PLAYER_SENTENCE:
            ps = parse_player_sentence(body)
            players[ps['guid']] = ps
        elif tid == TYPE_SITUATION:
            sits.append(parse_situation(body))
    flags = {}
    for s in sits:
        for pg in s['players']:
            ps = players.get(pg)
            if not ps or not ps['sentence']:
                continue
            lst = by_sentence.setdefault(ps['sentence'], [])
            if s['hash'] not in lst:
                lst.append(s['hash'])
            f = flags.setdefault(s['hash'], {'water': 0, 'tired': 0, 'long': 0, 'lines': 0})
            f['lines'] += 1
            for k in ('water', 'tired', 'long'):
                f[k] += int(ps[k])
    prio = {}
    if priority_core:
        for tid, body in read_objects(priority_core):
            if tid == TYPE_PRIORITY_INFO and len(body) >= 27:
                p = parse_priority(body)
                prio[p['hash']] = p
    return by_sentence, prio, flags, [s['hash'] for s in sits]
