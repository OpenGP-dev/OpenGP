#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads OpenGP profiles (--profile, F10; see docs/profiling.md).

A profile is a folder (user/profiles/<date>_<time>/ by default). Every
command prints plain, compact text, so its output can be read by a person or
fed to a tool.

  profiles.py list                         the profiles in user/profiles
  profiles.py [P] summary                  the session at a glance (the default)
  profiles.py [P] scenes                   frame times by scene
  profiles.py [P] hitches [--top N]        the longest frames, with causes and hot functions
  profiles.py [P] frames [--from F] [--to F] [--fields a,b] [--every N] [--where EXPR]
  profiles.py [P] frame F                  everything about one frame
  profiles.py [P] events [--type T,...] [--from F] [--to F]
  profiles.py [P] risks                    likely graphical glitches
  profiles.py [P] cpu [--from F] [--to F] [--slow MS] [--thread NAME] [--top N] [--stacks N]
  profiles.py [P] system                   CPU and memory over time
  profiles.py [P] thumbs [--frame F] [--sheet OUT --from F --to F]
  profiles.py [P] captures                 frame captures (F9)
  profiles.py capture [C] [--at X,Y | --png X,Y] [--draw I] [--owner NAME] [--grep TEXT]
                                          a frame capture: who drew what, what covers a pixel
  profiles.py [P] compare OTHER            two profiles side by side
  profiles.py fields                       the per-frame fields and what they mean

P is a profile folder, a folder name in user/profiles, or "latest" (the
default). Frames are the game's displayed frame numbers (F). C is a capture
folder, or a capture's number in the latest profile; the default is the
newest capture (in user/profiles or user/captures).
"""

import argparse
import json
import os
import struct
import sys
from collections import Counter, defaultdict

HOME = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROFILES = os.path.join(HOME, 'user', 'profiles')

FLAGS = [(1, 'paused'), (2, 'menu'), (4, 'background'), (8, 'linked'), (16, 'hitch'), (32, 'mark'),
         (64, 'capture'), (128, 'thumb'), (256, 'vsync'), (512, 'fullscreen')]


def flag_text(v):
    return ','.join(name for bit, name in FLAGS if v & bit)


def pct(values, p):
    if not values:
        return 0
    s = sorted(values)
    return s[min(len(s) - 1, int(p * len(s)))]


def ms(us):
    return us / 1000.0


def short(name):
    if name.startswith('weak.') and '.default.' in name:
        # A weak alias: "weak.?tolower...default.?__imp__fn_80270FE8guest..." -> "tolower (fn_80270FE8)"
        alias, target = name[5:].split('.default.', 1)

        def bare(s):
            s = s.lstrip('?')
            if s.startswith('__imp__'):
                s = s[7:]
            return s.split('guest')[0]
        return '%s (%s)' % (bare(alias), bare(target))
    for prefix in ('guest::', '__imp__'):
        if name.startswith(prefix):
            name = name[len(prefix):]
    return name


def is_wait(name):
    if not (name.startswith('Nt') or name.startswith('Zw')):
        return False
    return any(w in name for w in ('Wait', 'Delay', 'Alert', 'Remove', 'Yield', 'Signal'))


class Profile:
    def __init__(self, path):
        self.path = path
        with open(os.path.join(path, 'session.json'), encoding='utf-8') as f:
            self.session = json.load(f)
        rec = self.session['frame_record']
        self.fields = ['t_us'] + [f['name'] for f in rec['fields']]
        self.docs = {f['name']: f['doc'] for f in rec['fields']}
        self.struct = struct.Struct(rec['struct'])
        self._frames = None
        self._events = None
        self._system = None
        self.summary_json = self._json('summary.json')

    def _json(self, name):
        try:
            with open(os.path.join(self.path, name), encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    @property
    def frames(self):
        """Per-frame records as dicts (a frame recorded twice, after a restart, keeps the last)."""
        if self._frames is None:
            data = open(os.path.join(self.path, 'frames.bin'), 'rb').read()
            n = len(data) // self.struct.size
            names = self.fields
            self._frames = [dict(zip(names, v)) for v in self.struct.iter_unpack(data[:n * self.struct.size])]
            prev = None
            for r in self._frames:
                r['drops'] = 0 if prev is None else max(0, r['retrace'] - prev['retrace'] - 1)
                prev = r
        return self._frames

    @property
    def events(self):
        if self._events is None:
            self._events = []
            with open(os.path.join(self.path, 'events.jsonl'), encoding='utf-8', errors='replace') as f:
                for line in f:
                    try:
                        self._events.append(json.loads(line))
                    except ValueError:
                        pass  # a line cut short by a crash
        return self._events

    @property
    def system(self):
        if self._system is None:
            self._system = []
            try:
                with open(os.path.join(self.path, 'system.jsonl'), encoding='utf-8') as f:
                    for line in f:
                        try:
                            self._system.append(json.loads(line))
                        except ValueError:
                            pass
            except OSError:
                pass
        return self._system

    def scene_names(self):
        """Scene number -> a description from the scene events."""
        names = {}
        for e in self.events:
            if e['e'] == 'scene':
                text = e.get('kind', '')
                for key in ('activity', 'course'):
                    if e.get(key):
                        text += ': ' + e[key]
                names.setdefault(e['scene'], text)
        return names

    def thumbs(self):
        d = os.path.join(self.path, 'thumbs')
        out = []
        if os.path.isdir(d):
            for name in os.listdir(d):
                if name.startswith('f') and name.endswith('.jpg'):
                    try:
                        out.append((int(name[1:-4]), os.path.join(d, name)))
                    except ValueError:
                        pass
        return sorted(out)

    def nearest_thumb(self, frame):
        best = None
        for f, path in self.thumbs():
            if best is None or abs(f - frame) < abs(best[0] - frame):
                best = (f, path)
        return best

    # ---- CPU samples ----

    def cpu_runs(self):
        d = os.path.join(self.path, 'cpu')
        return sorted(os.path.join(d, r) for r in os.listdir(d)) if os.path.isdir(d) else []

    def load_cpu(self, run_dir):
        threads = {}
        try:
            with open(os.path.join(run_dir, 'threads.tsv'), encoding='utf-8', errors='replace') as f:
                for line in f:
                    p = line.rstrip('\n').split('\t')
                    if len(p) >= 3:
                        threads[int(p[0])] = p[2]
        except OSError:
            pass
        names = {}
        try:
            with open(os.path.join(run_dir, 'symbols.tsv'), encoding='utf-8', errors='replace') as f:
                for line in f:
                    p = line.rstrip('\n').split('\t')
                    if len(p) >= 3:
                        names[int(p[0])] = p[2]
        except OSError:
            pass
        stacks = []
        try:
            data = open(os.path.join(run_dir, 'stacks.bin'), 'rb').read()
            i = 0
            while i + 2 <= len(data):
                (depth,) = struct.unpack_from('<H', data, i)
                i += 2
                if i + 4 * depth > len(data):
                    break
                stacks.append(struct.unpack_from('<%dI' % depth, data, i))
                i += 4 * depth
        except OSError:
            pass
        try:
            data = open(os.path.join(run_dir, 'samples.bin'), 'rb').read()
        except OSError:
            data = b''
        n = len(data) // 8
        samples = struct.iter_unpack('<II', data[:n * 8])
        return threads, names, stacks, samples


def resolve(arg):
    if arg in (None, 'latest'):
        if not os.path.isdir(PROFILES):
            sys.exit('no profiles in ' + PROFILES)
        dirs = [os.path.join(PROFILES, d) for d in os.listdir(PROFILES)
                if os.path.isfile(os.path.join(PROFILES, d, 'session.json'))]
        if not dirs:
            sys.exit('no profiles in ' + PROFILES)
        return max(dirs, key=lambda d: os.path.getmtime(os.path.join(d, 'session.json')))
    if os.path.isfile(os.path.join(arg, 'session.json')):
        return arg
    if os.path.isfile(os.path.join(PROFILES, arg, 'session.json')):
        return os.path.join(PROFILES, arg)
    sys.exit('not a profile: ' + arg)


def in_range(r, args):
    f = r['frame']
    return (args.get('from') is None or f >= args['from']) and (args.get('to') is None or f <= args['to'])


# ---- commands ----

def cmd_list(_):
    if not os.path.isdir(PROFILES):
        print('no profiles in', PROFILES)
        return
    for d in sorted(os.listdir(PROFILES)):
        p = os.path.join(PROFILES, d)
        if not os.path.isfile(os.path.join(p, 'session.json')):
            continue
        size = os.path.getsize(os.path.join(p, 'frames.bin')) if os.path.exists(os.path.join(p, 'frames.bin')) else 0
        s = None
        try:
            s = json.load(open(os.path.join(p, 'summary.json'), encoding='utf-8'))
        except (OSError, ValueError):
            pass
        info = '%d frames, %d hitches, %.0f s' % (s['frames'], s['hitches'], s['seconds']) if s else '(no summary: still recording, or ended abruptly)'
        print('%-22s %8.1f MB frames  %s' % (d, size / 1e6, info))


def frame_stats(frames):
    live = [r for r in frames if not r['flags'] & 1]
    g = [r['guest_us'] for r in live if r['guest_us']]
    p = [r['present_us'] for r in live if r['present_us']]
    return live, g, p


def cmd_summary(prof, _args):
    s = prof.session
    print('profile  ', prof.path)
    print('game     ', s['game']['title'], '(%s)' % s['game']['id'], '| OpenGP', s.get('opengp'), s.get('commit', ''))
    sysinfo = s.get('system', {})
    print('system   ', sysinfo.get('os', '?'), '|', sysinfo.get('cpu', '?'), '| %s cores, %s MB' % (sysinfo.get('cores'), sysinfo.get('ram_mb')),
          '|', sysinfo.get('build', ''))
    starts = [e for e in prof.events if e['e'] in ('start', 'resume')]
    if starts:
        gpu = starts[0].get('gpu', {})
        host = starts[0].get('host', {})
        print('gpu      ', gpu.get('name', '?'), '(%s, %s)' % (gpu.get('api'), gpu.get('driver')), '|',
              host.get('display', ''), '| window', host.get('window', ''), '| headless' if host.get('headless') else '')
    settings = [e for e in prof.events if e['e'] == 'settings']
    if settings:
        st = settings[-1]
        print('settings ', 'aspect %s, internal %s, vsync %s, fullscreen %s, camera %s' % (
            st.get('aspect'), st.get('internal_resolution'), st.get('vsync'), st.get('fullscreen'), st.get('camera')))
    efb = [e for e in prof.events if e['e'] == 'efb']
    if efb:
        print('efb      ', ', '.join('%dpx wide at %dx (from frame %d)' % (e['width'], e['scale'], e['f']) for e in efb[-3:]))
    frames = prof.frames
    if not frames:
        print('no frames recorded')
        return
    live, g, p = frame_stats(frames)
    secs = (frames[-1]['t_us'] - frames[0]['t_us']) / 1e6
    runs = len(starts)
    print('recorded  %d frames in %.1f s (%d run%s)%s' % (len(frames), secs, runs, '' if runs == 1 else 's',
          '' if prof.summary_json else '; no summary.json: ended abruptly or still recording'))
    if secs > 0:
        retraces = frames[-1]['retrace'] - frames[0]['retrace']
        print('rate      %.2f fps presented, game at %.2f fps of %.2f retraces/s' % (
            len(frames) / secs, len(frames) / max(retraces, 1) * 59.94, retraces / secs))
    print('frame ms  game: mean %.2f p50 %.2f p90 %.2f p99 %.2f max %.1f | presented: p50 %.2f p99 %.2f max %.1f' % (
        ms(sum(g) / max(len(g), 1)), ms(pct(g, .5)), ms(pct(g, .9)), ms(pct(g, .99)), ms(max(g or [0])),
        ms(pct(p, .5)), ms(pct(p, .99)), ms(max(p or [0]))))
    drops = sum(r['drops'] for r in live)
    print('drops     %d retraces without a new frame (%.1f%% of frames late)' % (
        drops, 100.0 * sum(1 for r in live if r['drops']) / max(len(live), 1)))
    busy = [r['guest_us'] - r['idle_us'] - r['wait_us'] - r['readback_wait_us'] for r in live if r['guest_us']]
    if busy:
        print('game busy p50 %.2f ms, p90 %.2f ms, p99 %.2f ms of its frame (idle: spare time); GX front end p50 %.2f ms; renderer p50 %.2f ms, swapchain wait p50 %.2f ms' % (
            ms(pct(busy, .5)), ms(pct(busy, .9)), ms(pct(busy, .99)), ms(pct([r['gx_us'] for r in live], .5)),
            ms(pct([r['render_us'] for r in live], .5)), ms(pct([r['acquire_us'] for r in live], .5))))
    hitches = [e for e in prof.events if e['e'] == 'hitch']
    causes = Counter(e['cause'] for e in hitches)
    print('hitches   %d: %s' % (len(hitches), ', '.join('%s %d' % kv for kv in causes.most_common())))
    risks = [e for e in prof.events if e['e'] == 'risk']
    if risks:
        print('risks     %d distinct likely glitches: %s' % (len(risks), ', '.join('%s %d' % kv for kv in Counter(e['kind'] for e in risks).most_common())))
    pipes = [e for e in prof.events if e['e'] == 'pipelines']
    if pipes:
        print('shaders   %d pipelines created, %.1f ms in all, the slowest batch %.1f ms (frame %d)' % (
            sum(e['count'] for e in pipes), sum(e['ms'] for e in pipes), max(e['ms'] for e in pipes),
            max(pipes, key=lambda e: e['ms'])['f']))
    sysl = prof.system
    if sysl:
        sc = [x['system_cpu'] for x in sysl if 'system_cpu' in x]
        pc = [x['process_cpu'] for x in sysl if 'process_cpu' in x]
        mem = [x.get('private_mb', x.get('max_rss_mb', 0)) for x in sysl]
        pf = [x.get('page_faults', 0) for x in sysl]
        print('cpu       OpenGP %.0f%% of a core on average (max %.0f%%); the whole computer %.0f%% (max %.0f%%)' % (
            sum(pc) / max(len(pc), 1), max(pc or [0]), sum(sc) / max(len(sc), 1), max(sc or [0])))
        print('memory    up to %.0f MB; %.0f page faults/s on average' % (max(mem or [0]), sum(pf) / max(len(pf), 1)))
    marks = [e for e in prof.events if e['e'] == 'mark']
    if marks:
        print('marks     ' + ', '.join('frame %d (%s)' % (e['f'], e.get('note', '')) for e in marks))
    caps = [e for e in prof.events if e['e'] == 'capture']
    if caps:
        print('captures  ' + ', '.join('frame %d %s' % (e['f'], e.get('dir', '')) for e in caps))
    other = Counter(e['e'] for e in prof.events if e['e'] in ('hang', 'crash', 'link_lost', 'gpu_failures'))
    if other:
        print('problems  ' + ', '.join('%s %d' % kv for kv in other.items()))
    print()
    cmd_scenes(prof, {}, limit=12)
    if hitches:
        print()
        print('worst hitches (profiles.py hitches for more):')
        for e in sorted(hitches, key=lambda e: -max(e['guest_ms'], e['present_ms']))[:5]:
            print('  ' + hitch_line(e, prof.scene_names()))


def cmd_scenes(prof, _args, limit=None):
    names = prof.scene_names()
    by = defaultdict(list)
    for r in prof.frames:
        if not r['flags'] & 1:
            by[r['scene']].append(r)
    rows = []
    for scene, rs in by.items():
        g = [r['guest_us'] for r in rs if r['guest_us']]
        busy = [r['guest_us'] - r['idle_us'] - r['wait_us'] for r in rs if r['guest_us']]
        rows.append((len(rs), scene, g, busy, rs))
    rows.sort(key=lambda x: -x[0])
    print('scene  frames   mean    p99    max  late%  busy50  draws50  tris50  hitches  description')
    for n, scene, g, busy, rs in rows[:limit]:
        late = 100.0 * sum(1 for r in rs if r['drops']) / max(len(rs), 1)
        print('%5d %7d %6.2f %6.2f %6.1f %6.1f %7.2f %8d %7d %8d  %s' % (
            scene, n, ms(sum(g) / max(len(g), 1)), ms(pct(g, .99)), ms(max(g or [0])), late, ms(pct(busy, .5)),
            pct([r['draws'] for r in rs], .5), pct([r['triangles'] for r in rs], .5),
            sum(1 for r in rs if r['flags'] & 16), names.get(scene, '')))


def hitch_line(e, names):
    parts = ', '.join('%s %.1f' % kv for kv in sorted(e.get('ms', {}).items(), key=lambda kv: -kv[1]))
    text = 'frame %d: %.1f ms (game %.1f, presented %.1f; expected %.1f) %s [%s] scene %s%s' % (
        e['f'], max(e['guest_ms'], e['present_ms']), e['guest_ms'], e['present_ms'], e['expected_ms'], e['cause'], parts,
        e.get('scene'), ' (' + names.get(e.get('scene'), '') + ')' if names.get(e.get('scene')) else '')
    if e.get('after_scene_change'):
        text += ' after a scene change'
    if e.get('decodes'):
        text += ', %d textures decoded' % e['decodes']
    if e.get('system_cpu') is not None and e['system_cpu'] > 60:
        text += ', computer %.0f%% busy' % e['system_cpu']
    if e.get('self'):
        text += '\n      self: ' + ', '.join('%s %d%%' % (short(n), s) for n, s in e['self'])
    if e.get('inclusive'):
        text += '\n      incl: ' + ', '.join('%s %d%%' % (short(n), s) for n, s in e['inclusive'])
    return text


def cmd_hitches(prof, args):
    names = prof.scene_names()
    hitches = [e for e in prof.events if e['e'] == 'hitch' and in_range({'frame': e['f']}, args)]
    if args.get('scene') is not None:
        hitches = [e for e in hitches if e.get('scene') == args['scene']]
    print('%d hitches; by cause: %s' % (len(hitches), ', '.join('%s %d' % kv for kv in Counter(e['cause'] for e in hitches).most_common())))
    order = hitches if args.get('chrono') else sorted(hitches, key=lambda e: -max(e['guest_ms'], e['present_ms']))
    for e in order[:args.get('top') or 20]:
        print(hitch_line(e, names))


def cmd_fields(_args):
    p = Profile(resolve(None)) if os.path.isdir(PROFILES) else None
    if not p:
        sys.exit('no profile to read the fields from')
    print('t_us: microseconds since the session started, when the frame was presented')
    print('drops: (computed) retraces that went by without a new frame')
    for name in p.fields[1:]:
        print('%s: %s' % (name, p.docs.get(name, '')))


def cmd_frames(prof, args):
    fields = args.get('fields') or 'frame,flags,scene,guest_us,idle_us,gx_us,render_us,acquire_us,draws,triangles,decodes,drops'
    fields = fields.split(',')
    where = args.get('where')
    every = args.get('every') or 1
    rows = [r for r in prof.frames if in_range(r, args)]
    if where:
        code = compile(where, '<where>', 'eval')
        rows = [r for r in rows if eval(code, {}, dict(r))]
    rows = rows[::every]
    print(' '.join('%10s' % f[:10] for f in fields))
    for r in rows[:args.get('limit') or 400]:
        print(' '.join('%10s' % (flag_text(r[f]) if f == 'flags' else r[f]) for f in fields))
    if len(rows) > (args.get('limit') or 400):
        print('... %d more (use --limit, --every or --from/--to)' % (len(rows) - (args.get('limit') or 400)))


def cmd_frame(prof, args):
    f = args['frame']
    frames = prof.frames
    match = [r for r in frames if r['frame'] == f]
    if not match:
        sys.exit('frame %d not recorded (frames %d-%d)' % (f, frames[0]['frame'], frames[-1]['frame']))
    r = match[-1]
    names = prof.scene_names()
    print('frame %d at %.3f s, scene %d (%s), flags %s' % (f, r['t_us'] / 1e6, r['scene'], names.get(r['scene'], ''), flag_text(r['flags'])))
    for name in prof.fields[1:]:
        if name in ('frame', 'flags'):
            continue
        print('  %-18s %10d   %s' % (name, r[name], prof.docs.get(name, '')))
    print('  %-18s %10d   %s' % ('drops', r['drops'], 'retraces without a new frame before it'))
    near = [e for e in prof.events if abs(e['f'] - f) <= 2]
    if near:
        print('events near it:')
        for e in near:
            print('  ' + json.dumps(e)[:400])
    t = prof.nearest_thumb(f)
    if t:
        print('thumbnail: %s (frame %d)' % (t[1], t[0]))
    if prof.cpu_runs():
        print('cpu during it (profiles.py cpu --from %d --to %d for more):' % (f, f))
        cmd_cpu(prof, {'from': f, 'to': f, 'top': 8})


def cmd_events(prof, args):
    types = set(args['type'].split(',')) if args.get('type') else None
    n = 0
    for e in prof.events:
        if types and e['e'] not in types:
            continue
        if not in_range({'frame': e['f']}, args):
            continue
        print(json.dumps(e)[:args.get('width') or 600])
        n += 1
        if n >= (args.get('limit') or 400):
            print('... (use --limit, --type or --from/--to)')
            break


def cmd_risks(prof, _args):
    risks = [e for e in prof.events if e['e'] == 'risk']
    counts = {}
    if prof.summary_json:
        for r in prof.summary_json.get('risks', []):
            counts[(r['kind'], r['address'], r['a'], r['b'])] = r['count']
    if not risks:
        print('no likely glitches noted')
        return
    names = prof.scene_names()
    for e in risks:
        n = counts.get((e['kind'], e['address'], e['a'], e['b']))
        print('frame %d: %s at %s (a=%s b=%s)%s scene %s %s\n    %s' % (
            e['f'], e['kind'], e['address'], e['a'], e['b'], ', seen %d times' % n if n else '', e.get('scene'),
            names.get(e.get('scene'), ''), e.get('what', '')))


def cmd_cpu(prof, args):
    runs = prof.cpu_runs()
    if not runs:
        print('no CPU samples (Windows only; --profile-rate 0 turns them off)')
        return
    # Time windows (100 us units) of the frames asked for: from the game
    # finishing the frame before to it finishing this one.
    windows = None
    if args.get('from') is not None or args.get('to') is not None or args.get('slow'):
        windows = []
        for r in prof.frames:
            if not in_range(r, args):
                continue
            if args.get('slow') and r['guest_us'] < args['slow'] * 1000:
                continue
            end = r['t_us'] - r['latency_us']
            windows.append(((end - r['guest_us']) // 100, end // 100 + 1))
        windows.sort()
        if not windows:
            print('no frames match')
            return
    thread_filter = args.get('thread') or 'guest'
    self_counts, incl_counts, stack_counts = Counter(), Counter(), Counter()
    total = waits = 0
    import bisect
    starts = [w[0] for w in windows] if windows else None
    for run in runs:
        threads, names, stacks, samples = prof.load_cpu(run)
        wanted = {i for i, n in threads.items() if n.startswith(thread_filter) or thread_filter == 'all'}
        for t, word in samples:
            thread, stack = word >> 24, word & 0xFFFFFF
            if thread not in wanted or stack >= len(stacks):
                continue
            if windows is not None:
                i = bisect.bisect_right(starts, t) - 1
                if i < 0 or t >= windows[i][1]:
                    continue
            st = stacks[stack]
            top = names.get(st[0], '?')
            if is_wait(top):
                waits += 1
                continue
            total += 1
            self_counts[short(top)] += 1
            seen = set()
            for a in st:
                n = short(names.get(a, '?'))
                if n not in seen:
                    seen.add(n)
                    incl_counts[n] += 1
            if args.get('stacks'):
                stack_counts[tuple(short(names.get(a, '?')) for a in st[:12])] += 1
    if total == 0:
        print('no samples (threads matching %r)' % thread_filter)
        return
    top = args.get('top') or 25
    print('%d samples of %s threads at work (%d more blocked or idle)%s' % (
        total, thread_filter, waits, ' in %d frames' % len(windows) if windows else ''))
    print('self (where the time is spent):')
    for n, c in self_counts.most_common(top):
        print('  %5.1f%%  %s' % (100.0 * c / total, n))
    print('inclusive (on the stack):')
    for n, c in [x for x in incl_counts.most_common() if x[1] < total * 0.98][:top]:
        print('  %5.1f%%  %s' % (100.0 * c / total, n))
    if args.get('stacks'):
        print('hottest stacks (innermost first):')
        for st, c in stack_counts.most_common(args['stacks']):
            print('  %5.1f%%  %s' % (100.0 * c / total, ' <- '.join(st)))


def cmd_system(prof, args):
    sysl = prof.system
    if not sysl:
        print('no system samples')
        return
    every = args.get('every') or max(1, len(sysl) // 40)
    print('     t  frame  opengp%  computer%  private_mb  faults/s  threads (percent of a core)')
    for x in sysl[::every]:
        threads = ', '.join('%s %.0f' % (t['name'], t['cpu']) for t in sorted(x.get('threads', []), key=lambda t: -t['cpu'])[:6])
        print('%6.0f %6d %8.0f %10.0f %11.0f %9d  %s' % (x['t'], x['f'], x.get('process_cpu', 0), x.get('system_cpu', -1),
                                                    x.get('private_mb', x.get('max_rss_mb', 0)), x.get('page_faults', 0), threads))


def cmd_thumbs(prof, args):
    thumbs = prof.thumbs()
    if args.get('frame') is not None:
        t = prof.nearest_thumb(args['frame'])
        print(t[1] if t else 'no thumbnails')
        return
    if args.get('sheet'):
        try:
            from PIL import Image
        except ImportError:
            sys.exit('contact sheets need Pillow (pip install pillow)')
        chosen = [(f, p) for f, p in thumbs if in_range({'frame': f}, args)]
        if not chosen:
            sys.exit('no thumbnails in that range')
        step = max(1, len(chosen) // (args.get('count') or 24))
        chosen = chosen[::step][:args.get('count') or 24]
        images = [Image.open(p) for _, p in chosen]
        w, h = images[0].size
        cols = min(6, len(images))
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * w, rows * (h + 14)), 'black')
        from PIL import ImageDraw
        draw = ImageDraw.Draw(sheet)
        for i, (img, (f, _)) in enumerate(zip(images, chosen)):
            x, y = (i % cols) * w, (i // cols) * (h + 14)
            sheet.paste(img, (x, y + 14))
            draw.text((x + 2, y + 1), 'frame %d' % f, fill='white')
        sheet.save(args['sheet'])
        print('%s: %d thumbnails, frames %d-%d' % (args['sheet'], len(images), chosen[0][0], chosen[-1][0]))
        return
    print('%d thumbnails in %s' % (len(thumbs), os.path.join(prof.path, 'thumbs')))
    if thumbs:
        print('frames %d-%d' % (thumbs[0][0], thumbs[-1][0]))


def cmd_captures(prof, _args):
    d = os.path.join(prof.path, 'captures')
    caps = sorted(os.listdir(d)) if os.path.isdir(d) else []
    if not caps:
        print('no captures (F9 captures a frame)')
        return
    for c in caps:
        p = os.path.join(d, c)
        summary = os.path.join(p, 'capture.txt')
        head = ''
        if os.path.exists(summary):
            with open(summary, encoding='utf-8', errors='replace') as f:
                head = ' | '.join(line.strip() for line in [next(f, ''), next(f, '')])
        print('%s  %s' % (p, head))


def find_capture(arg):
    """A capture folder from an argument (see the help)."""
    if arg and os.path.isfile(os.path.join(arg, 'capture.json')):
        return arg
    candidates = []
    for root in (PROFILES, os.path.join(HOME, 'user', 'captures')):
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            if 'capture.json' in filenames:
                candidates.append(dirpath)
                dirnames[:] = []
    if arg:
        named = [c for c in candidates if os.path.basename(c) == arg or os.path.basename(c).startswith(arg.zfill(3) + '_')]
        if named:
            return max(named, key=os.path.getmtime)
        sys.exit('no capture ' + arg)
    if not candidates:
        sys.exit('no captures (F9 captures a frame)')
    return max(candidates, key=lambda c: os.path.getmtime(os.path.join(c, 'capture.json')))


def capture_blocks(path):
    """capture.txt's command blocks by command index."""
    blocks = {}
    current = None
    with open(os.path.join(path, 'capture.txt'), encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                current = int(line[1:].split(' ', 1)[0])
                blocks[current] = [line.rstrip('\n')]
            elif current is not None and line.startswith('    '):
                blocks[current].append(line.rstrip('\n'))
            else:
                current = None
    return blocks


def cmd_capture(argv):
    ap = argparse.ArgumentParser(prog='profiles.py capture')
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--at', help='X,Y in game pixels (0-640 x 0-528; widescreen beyond 0 and 640)')
    ap.add_argument('--png', help='X,Y in pixels of screen.png / efb_end.png')
    ap.add_argument('--draw', type=int, help='a command number: its full description')
    ap.add_argument('--owner', help='draws owned by this function')
    ap.add_argument('--grep', help='commands whose description contains this text')
    ap.add_argument('--top', type=int, default=25)
    args = ap.parse_args(argv)
    path = find_capture(args.capture)
    cap = json.load(open(os.path.join(path, 'capture.json'), encoding='utf-8'))
    cmds = cap['commands']
    draws = [c for c in cmds if c['type'] == 'draw']
    if args.draw is not None:
        blocks = capture_blocks(path)
        print('\n'.join(blocks.get(args.draw, ['no command %d' % args.draw])))
        return
    if args.at or args.png:
        x, y = (float(v) for v in (args.at or args.png).split(','))
        if args.png:
            # screen.png and efb_end.png are at the internal resolution; the
            # wide EFB starts `side` pixels left of the game's 0.
            x = x / cap['scale'] - cap['side']
            y = y / cap['scale']
        hits = [c for c in draws if 'bounds' in c and c['bounds'][0] <= x <= c['bounds'][2] and c['bounds'][1] <= y <= c['bounds'][3]]
        print('%d draws cover game pixel %.1f,%.1f (by their bounding boxes; the last drawn is on top):' % (len(hits), x, y))
        for c in reversed(hits[-args.top:]):
            tex = ', '.join('%s %s %dx%d' % (t.get('address', '?'), t.get('format', '?'), t.get('width', 0), t.get('height', 0)) for t in c.get('textures', []))
            print('  #%d %s %d tris z %.6f..%.6f | %s | %s | owner %s%s' % (
                c['i'], c.get('primitive', ''), c['triangles'], c['depth'][0], c['depth'][1], c['pipeline'], tex or 'no texture',
                c.get('owner', '?'), ' | by ' + ' <- '.join(c['by'][:3]) if c.get('by') else ''))
        if len(hits) > args.top:
            print('  ... %d more underneath (--top)' % (len(hits) - args.top))
        return
    if args.owner or args.grep:
        blocks = capture_blocks(path) if args.grep else None
        n = 0
        for c in cmds:
            if args.owner and c.get('owner') != args.owner:
                continue
            if args.grep and args.grep not in '\n'.join(blocks.get(c['i'], [])):
                continue
            n += 1
            if n <= args.top:
                if blocks:
                    print('\n'.join(blocks.get(c['i'], [])))
                else:
                    print('  #%d %s %d tris bounds %s z %s | %s' % (c['i'], c.get('primitive', c['type']), c.get('triangles', 0),
                                                                   c.get('bounds'), c.get('depth'), c.get('pipeline', '')))
        print('%d commands match' % n)
        return
    with open(os.path.join(path, 'capture.txt'), encoding='utf-8', errors='replace') as f:
        head = []
        for line in f:
            if line.startswith('#'):
                break
            head.append(line.rstrip('\n'))
    print('capture  ', path)
    print('\n'.join(head))
    files = sorted(os.listdir(path))
    print('files:', ', '.join(f + ('/ (%d)' % len(os.listdir(os.path.join(path, f))) if os.path.isdir(os.path.join(path, f)) else '') for f in files))


def cmd_compare(prof, args):
    other = Profile(resolve(args['other']))
    rows = []
    for p in (prof, other):
        live, g, pr = frame_stats(p.frames)
        secs = (p.frames[-1]['t_us'] - p.frames[0]['t_us']) / 1e6 if p.frames else 0
        busy = [r['guest_us'] - r['idle_us'] - r['wait_us'] for r in live if r['guest_us']]
        rows.append({
            'profile': os.path.basename(p.path.rstrip('/\\')),
            'frames': len(p.frames),
            'fps': len(p.frames) / secs if secs else 0,
            'game ms p50': ms(pct(g, .5)), 'game ms p99': ms(pct(g, .99)), 'game ms max': ms(max(g or [0])),
            'late %': 100.0 * sum(1 for r in live if r['drops']) / max(len(live), 1),
            'busy ms p50': ms(pct(busy, .5)), 'busy ms p90': ms(pct(busy, .9)),
            'gx ms p50': ms(pct([r['gx_us'] for r in live], .5)),
            'render ms p50': ms(pct([r['render_us'] for r in live], .5)),
            'hitches': sum(1 for e in p.events if e['e'] == 'hitch'),
            'risks': sum(1 for e in p.events if e['e'] == 'risk'),
        })
    keys = list(rows[0].keys())
    for k in keys:
        a, b = rows[0][k], rows[1][k]
        if isinstance(a, float):
            print('%-14s %12.2f %12.2f   %+.2f' % (k, a, b, b - a))
        else:
            print('%-14s %12s %12s' % (k, a, b))


def main():
    argv = sys.argv[1:]
    if argv and argv[0] in ('-h', '--help'):
        print(__doc__)
        return
    if argv and argv[0] == 'list':
        cmd_list(None)
        return
    if argv and argv[0] == 'fields':
        cmd_fields(None)
        return
    if argv and argv[0] == 'capture':
        cmd_capture(argv[1:])
        return
    commands = {'summary', 'scenes', 'hitches', 'frames', 'frame', 'events', 'risks', 'cpu', 'system', 'thumbs',
                'captures', 'compare'}
    path = None
    if argv and argv[0] not in commands:
        path = argv.pop(0)
    command = argv.pop(0) if argv else 'summary'
    if command not in commands:
        sys.exit(__doc__)
    ap = argparse.ArgumentParser(prog='profiles.py ' + command)
    if command == 'frame':
        ap.add_argument('frame', type=int)
    if command == 'compare':
        ap.add_argument('other')
    ap.add_argument('--from', dest='from', type=int)
    ap.add_argument('--to', type=int)
    ap.add_argument('--top', type=int)
    ap.add_argument('--limit', type=int)
    ap.add_argument('--every', type=int)
    ap.add_argument('--fields')
    ap.add_argument('--where')
    ap.add_argument('--type')
    ap.add_argument('--scene', type=int)
    ap.add_argument('--chrono', action='store_true')
    ap.add_argument('--slow', type=float, help='only frames slower than this many ms')
    ap.add_argument('--thread')
    ap.add_argument('--stacks', type=int)
    ap.add_argument('--sheet')
    ap.add_argument('--count', type=int)
    ap.add_argument('--width', type=int)
    if command == 'thumbs':
        ap.add_argument('--frame', type=int)
    args = vars(ap.parse_args(argv))
    prof = Profile(resolve(path))
    globals()['cmd_' + command](prof, args)


if __name__ == '__main__':
    main()
