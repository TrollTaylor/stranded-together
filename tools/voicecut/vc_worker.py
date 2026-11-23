"""voicecut: background decoding in worker processes (concurrent.futures, stdlib). Each worker opens the game once
(the archive index and Wwise's codebook library from ds.exe) and decodes catalog items straight into the workspace's
audio cache, so the editor never waits for a line it has not opened yet.
"""
import os

import vc_audio as AU

_STATE = {}


def init(game_dir, index_dir):
    import vc_codebooks
    import vc_game
    _STATE['gd'] = vc_game.GameData(game_dir, index_dir)
    with open(os.path.join(game_dir, 'ds.exe'), 'rb') as f:
        _STATE['lib'] = vc_codebooks.load_library(f.read())


def decode_pcm(item):
    import vc_vorbis as V
    data = _STATE['gd'].stream(item['stream'])
    w = V.parse_wem(data)
    w, pcm = V.decode_wem(data, _STATE['lib'], mix=V.voice_mix(w))
    return w.rate, pcm[0]


def write_cache(path, rate, pcm):
    """Write a decoded source to the cache atomically (a private temp name, then a rename) and return its stats."""
    tmp = '%s.%d.tmp' % (path, os.getpid())
    AU.write_wav(tmp, pcm, rate)
    os.replace(tmp, path)
    return AU.stats(pcm, rate)


def analyse(kind, path, subs=None, item_id=''):
    """vc_match on a cached source: a cutscene's candidate sounds of Fragile, or the features of one of Sam's lines."""
    import vc_match
    if kind == 'cutscene':
        return vc_match.analyse_cutscene(path, subs or [], item_id)
    return vc_match.analyse_line(path)


def decode_to(item, path):
    if os.path.exists(path):
        rate, pcm = AU.read_wav(path)
        return AU.stats(pcm, rate)
    rate, pcm = decode_pcm(item)
    return write_cache(path, rate, pcm)
