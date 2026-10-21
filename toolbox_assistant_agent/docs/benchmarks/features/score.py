#!/usr/bin/env python3
"""Score assistant replies to PlotJuggler product questions against a feature-catalog fixture.

Input: a JSONL file, one reply per line:
    {"question_id", "fixture", "backend", "model", "rep", "reply", "tool_calls"}
`tool_calls` is a count, a list of tool names, or a list of {"name": ...} objects.

Usage:
    python3 -I score.py results.jsonl [--questions questions.json] [--fixtures DIR]
    python3 -I score.py --self-test

Hard failures (the reply fails):
    no_entry_label          navigation: no accepted label of an expected entry
    install_for_loaded      a loaded/core feature, but the reply tells the user to install something
    no_marketplace          installable/unlisted feature, but the reply never mentions the Marketplace
    no_enable               a disabled extension, but the reply never says to enable it
    unsupported_not_stated  a limitation, but the reply does not say it is unsupported
    negated_supported       a supported feature described as impossible ("cannot show images")
    premature_external      a non-PlotJuggler tool named before any PlotJuggler route
    tool_calls_nonzero      a navigation question answered with tool calls
    invented_step           strict_steps question: a UI path segment not in the fixture, a forbidden step,
                            or a path whose segments, each known, follow no fixture entry_point in order
Flags (manual review, not failures): unknown UI path segments, unknown quoted labels, and an
external tool named after the PlotJuggler route.

Standard library only.
"""

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent

EXTERNAL_TOOLS = {
    "Foxglove": r"\bfoxglove\b(?![ _-]*(?:bridge|websocket|ws\b|protocol|schema|message|server))",
    "Lichtblick": r"\blichtblick\b",
    "RViz": r"\brviz2?\b",
    "rqt": r"\brqt(?:_plot)?\b",
    "PlotJuggler 3": r"\bplotjuggler\s*3\b|\bpj3\b",
    "MATLAB": r"\bmatlab\b",
    "Grafana": r"\bgrafana\b",
    "OBS": r"\bobs(?: studio)?\b",
    "ffmpeg": r"\bffmpeg\b",
    "VLC": r"\bvlc\b",
    "Rerun": r"\brerun(?:\.io)?\b",
    "Python": r"\bpython\b|\bpandas\b|\bmatplotlib\b|\bjupyter\b",
    "Excel": r"\bexcel\b",
    "LibreOffice": r"\blibreoffice\b",
    "ros2 CLI": r"\bros2 (?:topic|bag|service|action)\b|\brostopic\b|\brosbag\b",
    "MQTT Explorer": r"\bmqtt explorer\b|\bmosquitto_sub\b",
    "Wireshark": r"\bwireshark\b",
}

NEG = (
    r"(?:can ?not|can't|cannot|unable to|not (?:possible|supported|able to)|no (?:way|support) (?:to|for)"
    r"|does(?:n't| not) support|is(?:n't| not) supported|unsupported)"
)
NEG_RE = re.compile(NEG)
INSTALL_RE = re.compile(r"\b(?:install|installing|download)\b")
INSTALL_EXEMPT = ("already ", "no need to ", "don't need to ", "do not need to ", "without ", "nothing to ")
ENABLE_RE = re.compile(r"\b(?:re-?)?enabl(?:e|ed|ing)\b|\b(?:turn|switch) (?:it )?(?:back )?on\b|\btoggle\b")
QUOTE_RE = re.compile(r"\"([^\"\n]{2,60})\"|`([^`\n]{2,60})`|\*\*([^*\n]{2,60})\*\*")


def norm(text: str) -> str:
    text = text.lower()
    for a, b in (("“", '"'), ("”", '"'), ("‘", "'"), ("’", "'"), ("...", "…")):
        text = text.replace(a, b)
    text = re.sub(r"\s*(?:->|=>|⇒|›|▸|»|→| > )\s*", " → ", text)
    return re.sub(r"[ \t]+", " ", text)


def norm_label(label: str) -> str:
    return norm(label).replace("*", "").replace("`", "").strip().rstrip("…").strip()


def tool_names(tool_calls):
    if isinstance(tool_calls, int):
        return [""] * tool_calls
    if isinstance(tool_calls, list):
        return [c.get("name", "") if isinstance(c, dict) else str(c) for c in tool_calls]
    return []


def per_fixture(value, fixture):
    return value.get(fixture) if isinstance(value, dict) else value


def fixture_entries(fixture: dict) -> list[dict]:
    return (fixture.get("core", []) + fixture.get("loaded", []) + fixture.get("installed_not_loaded", [])
            + fixture.get("available", []))


def label_pieces(entry_point: str) -> set[str]:
    """The labels in a normalized entry_point (or a part of one)."""
    pieces = {p.strip().rstrip("…").strip() for p in re.split(r"→|/|\(|\)|,|;| or |\bthen\b", entry_point)}
    pieces.discard("")
    return pieces


def fixture_vocabulary(fixture: dict) -> set[str]:
    """Every label the fixture puts in front of the model: names and entry-point pieces."""
    vocab = set()
    for e in fixture_entries(fixture):
        vocab.add(norm_label(e.get("name", "")))
        vocab |= label_pieces(norm(e.get("entry_point", "")))
    vocab.discard("")
    return vocab


def segment_known(segment: str, vocab: set[str]) -> bool:
    seg = segment.strip(" .:!?\"'`*()").rstrip("…").strip()
    if not seg or seg in vocab:
        return True
    for label in vocab:
        if len(label) >= 4 and re.search(r"(?<!\w)" + re.escape(label) + r"(?!\w)", seg):
            return True
        if " " in seg and re.search(r"(?<!\w)" + re.escape(seg) + r"(?!\w)", label):
            return True
    return False


def entry_routes(fixture: dict) -> list[list[set[str]]]:
    """Every route the fixture lists: each entry_point split into its alternatives ("; ", ", or ",
    "(or ...)"), each alternative into its "→" segments, each segment into its label_pieces."""
    routes = []
    for e in fixture_entries(fixture):
        entry_point = re.sub(r"\(or ([^)]*)\)", r"; \1", norm(e.get("entry_point", "")))
        for alternative in re.split(r";|, or ", entry_point):
            route = [pieces for pieces in map(label_pieces, alternative.split("→")) if pieces]
            if route:
                routes.append(route)
    return routes


def route_paths(path: list[str]) -> list[list[str]]:
    """Splits a reply path where a middle segment holds a sentence break ("File → Open Data File.
    Then Sources panel → File"): the text before the break ends one route, the text after it
    starts the next."""
    out, current = [], [path[0]]
    for segment in path[1:-1]:
        parts = re.split(r"[.!?;]\s", segment)
        if len(parts) == 1:
            current.append(segment)
            continue
        out.append(current + [parts[0]])
        current = [re.split(r"[.!?:;]\s|,\s", parts[-1])[-1]]
    out.append(current + [path[-1]])
    return [p for p in out if len(p) >= 2]


def follows_route(path: list[str], route: list[set[str]]) -> bool:
    """The path's segments match the route's segments in order, possibly skipping some; two
    reply segments may fall in one route segment ("Empty plot area → 2D button")."""
    j = 0
    for segment in path:
        while j < len(route) and not segment_known(segment, route[j]):
            j += 1
        if j == len(route):
            return False
    return True


def ui_paths(text: str) -> list[list[str]]:
    paths = []
    for line in text.splitlines():
        if "→" not in line:
            continue
        parts = line.split("→")
        first = re.split(r"[.!?:;]\s|,\s", parts[0])[-1]
        last = re.split(r"[.!?;,()]|\s—\s|\s-\s", parts[-1])[0]
        paths.append([first, *parts[1:-1], last])
    return paths


def quoted(text: str) -> list[str]:
    return [next(g for g in m.groups() if g) for m in QUOTE_RE.finditer(text)]


def first_index(text: str, needles) -> int:
    hits = [text.find(n) for n in needles if n and text.find(n) >= 0]
    return min(hits) if hits else -1


# "I can't say / promise / confirm ..." hedges what the model knows; it does not deny a feature.
HEDGE_RE = re.compile(r"^(?:can ?not|can't|cannot|unable to)\s+(?:say|tell|promise|confirm|see|check|verify|point|find"
                      r"|guarantee|be sure|reach|list)\b")
STOP_WORDS = {"plotjuggler", "cannot", "there", "which", "their", "about", "other", "these", "those", "would"}


def content_words(text: str) -> set[str]:
    return {w for w in re.findall(r"[a-z]{5,}", text) if w not in STOP_WORDS}


def sentence_at(text: str, pos: int) -> str:
    start = max(text.rfind(c, 0, pos) for c in ".!?\n") + 1
    ends = [i for i in (text.find(c, pos) for c in ".!?\n") if i >= 0]
    return text[start:min(ends) if ends else len(text)]


def negates(text: str, keywords, limitations=()) -> bool:
    """A supported feature described as impossible. Restating a known limitation of the
    fixture ("it cannot publish MQTT messages") and hedging ("I can't promise ...") are not."""
    limitation_words = [content_words(norm(s)) for s in limitations]
    for kw in keywords:
        k = re.escape(norm_label(kw))
        patterns = (NEG + r"[^.,;:!?\n]{0,40}?(?<!\w)" + k + r"(?!\w)",
                    r"(?<!\w)" + k + r"(?!\w)[^.,;:!?\n]{0,40}?(?:is(?:n't| not) (?:supported|possible|available)"
                    r"|unsupported)")
        for pattern in patterns:
            for m in re.finditer(pattern, text):
                if HEDGE_RE.match(m.group(0)):
                    continue
                words = content_words(sentence_at(text, m.start()))
                if any(len(words & lw) >= 2 for lw in limitation_words):
                    continue
                return True
    return False


def tells_to_install(text: str) -> bool:
    for m in INSTALL_RE.finditer(text):
        before = text[max(0, m.start() - 20):m.start()]
        if not any(before.endswith(x) for x in INSTALL_EXEMPT):
            return True
    return False


def score_reply(question: dict, fixture_name: str, fixture: dict, reply: str, tool_calls) -> dict:
    text = norm(reply)
    question_text = norm(question["text"])
    fails, flags = [], []
    names = tool_names(tool_calls)
    vocab = fixture_vocabulary(fixture)
    labels_by_entry = question.get("accepted_labels", {})
    expected = per_fixture(question.get("expect_entry_ids", []), fixture_name) or []
    accepted = [norm_label(lbl) for e in expected for lbl in labels_by_entry.get(e, [])]
    vocab |= {lbl for lbls in labels_by_entry.values() for lbl in map(norm_label, lbls)}
    availability = per_fixture(question.get("availability"), fixture_name)

    # With nothing loaded the assistant can only offer the tool it would call; that counts too.
    offered = any(norm_label(w) in text for w in question.get("accept_offer_words", []))
    by_tool = bool(set(names) & set(question.get("accept_tool_calls", []))) or offered
    label_hit = first_index(text, accepted) >= 0
    if expected and not (label_hit or by_tool):
        fails.append("no_entry_label")

    if not by_tool:
        if availability in ("loaded", "core") and tells_to_install(text):
            fails.append("install_for_loaded")
        if availability in ("installable", "unlisted") and "marketplace" not in text:
            fails.append("no_marketplace")
        if availability == "disabled" and not ENABLE_RE.search(text):
            fails.append("no_enable")
    if availability == "unsupported" and not NEG_RE.search(text) and "only receives" not in text:
        fails.append("unsupported_not_stated")
    limitations = [lim.get("summary", "") for lim in fixture.get("limitations", [])]
    if availability != "unsupported" and negates(text, question.get("negation_keywords", []), limitations):
        fails.append("negated_supported")

    # A route is any accepted label, or any entry name or entry-point piece the fixture lists.
    route_at = first_index(text, accepted + sorted(v for v in vocab if len(v) >= 6))
    entries = {e["id"]: e for key in ("core", "loaded", "installed_not_loaded", "available")
               for e in fixture.get(key, [])}
    expected_summaries = " ".join(norm(entries.get(e, {}).get("summary", "")) for e in expected)
    for tool, pattern in EXTERNAL_TOOLS.items():
        if re.search(pattern, question_text) or re.search(pattern, expected_summaries):
            continue  # named by the user or by the expected entry itself: not a redirect
        m = re.search(pattern, text)
        if not m or question.get("external_redirect_allowed"):
            continue
        if route_at < 0 or m.start() < route_at:
            fails.append(f"premature_external:{tool}")
        else:
            flags.append(f"external_after_route:{tool}")

    if question.get("category") == "navigation" and question.get("tool_calls_zero") and names:
        fails.append("tool_calls_nonzero")

    # Only an unknown or out-of-order "A → B" path or a forbidden step fails a strict question; an unknown
    # quoted or bold phrase is often emphasis or a heading, so it goes to manual review.
    invented = []
    routes = entry_routes(fixture) if question.get("strict_steps") else []
    for path in ui_paths(text):
        unknown = [f"path:{seg.strip()}" for seg in path if not segment_known(seg, vocab)]
        invented += unknown
        if unknown or not routes:
            continue
        # Each segment is known; a strict question also wants them in an order the fixture lists.
        for sub in route_paths(path):
            if not any(follows_route(sub, route) for route in routes):
                invented.append("route:" + " → ".join(seg.strip() for seg in sub))
    for phrase in question.get("forbidden_steps", []):
        for m in re.finditer(re.escape(norm_label(phrase)), text):
            sentence = re.split(r"[.!?\n]", text[:m.start()])[-1]
            if not re.search(r"\b(?:no|not|never|without)\b|n't|cannot", sentence):
                invented.append(f"forbidden:{phrase}")
                break
    if invented and question.get("strict_steps"):
        fails.append("invented_step")
    flags += invented
    for q in quoted(text):
        if "/" in q and " " not in q:
            continue  # a topic or file path, not a UI label
        if q in question_text or segment_known(q, vocab):
            continue
        flags.append(f"quoted:{q}")
    return {"pass": not fails, "fails": fails, "flags": flags}


def load_fixtures(directory: Path) -> dict:
    return {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in sorted(directory.glob("host_*.json"))}


def report(rows: list[dict]) -> None:
    groups = defaultdict(list)
    for r in rows:
        groups[(r["backend"], r["model"])].append(r)
    for (backend, model), items in sorted(groups.items()):
        print(f"\n== backend={backend} model={model}")
        print(f"{'question':28} {'fixture':14} {'pass':>7}  failures")
        cells = defaultdict(list)
        for r in items:
            cells[(r["question_id"], r["fixture"])].append(r)
        for (qid, fx), reps in sorted(cells.items()):
            answered = [r for r in reps if not r["result"].get("error")]
            passed = sum(r["result"]["pass"] for r in answered)
            reasons = sorted({f for r in answered for f in r["result"]["fails"]})
            if len(answered) < len(reps):
                reasons.append(f"{len(reps) - len(answered)} run error(s), not scored")
            print(f"{qid:28} {fx:14} {passed:>3}/{len(answered):<3}  {', '.join(reasons)}")
        answered = [r for r in items if not r["result"].get("error")]
        total = sum(r["result"]["pass"] for r in answered)
        errors = len(items) - len(answered)
        print(f"{'TOTAL':28} {'':14} {total:>3}/{len(answered):<3}" + (f"  ({errors} run errors excluded)" if errors else ""))
    flagged = [r for r in rows if r["result"]["flags"]]
    print(f"\n== flagged for manual review: {len(flagged)}")
    for r in flagged:
        excerpt = " ".join(r["reply"].split())[:160]
        print(f"- {r['question_id']} {r['fixture']} {r['backend']}/{r['model']} rep={r.get('rep')}: "
              f"{'; '.join(r['result']['flags'])}\n    {excerpt}")


def run(results_path: Path, questions_path: Path, fixtures_dir: Path) -> int:
    questions = {q["id"]: q for q in json.loads(questions_path.read_text(encoding="utf-8"))["questions"]}
    fixtures = load_fixtures(fixtures_dir)
    rows = []
    for n, line in enumerate(results_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        row = json.loads(line)
        q = questions.get(row["question_id"])
        fx = fixtures.get(row["fixture"])
        if q is None or fx is None:
            print(f"line {n}: unknown question {row['question_id']!r} or fixture {row['fixture']!r}", file=sys.stderr)
            return 2
        if not row.get("reply", "").strip():
            row["result"] = {"pass": False, "fails": [], "flags": [], "error": True}  # the CLI failed, no answer
        else:
            row["result"] = score_reply(q, row["fixture"], fx, row["reply"], row.get("tool_calls", 0))
        rows.append(row)
    report(rows)
    return 0


SELF_TEST = [
    # (question id, fixture, reply, tool_calls, expected pass, expected fail prefixes)
    ("Q01_show_image", "host_bundled",
     "Use the 2D view: in an empty plot area click 2D, or drag an image topic from the Datasets panel onto it.",
     0, True, []),
    ("Q01_show_image", "host_bundled",
     "PlotJuggler cannot show images. Use Foxglove Studio instead.", 0, False,
     ["no_entry_label", "negated_supported", "premature_external"]),
    ("Q02_mqtt", "host_bundled",
     "Install the MQTT Subscriber from File → Extensions Marketplace…, then Sources panel → Stream → "
     "MQTT Subscriber → Start streaming.", 0, True, []),
    ("Q02_mqtt", "host_bundled",
     "Install the MQTT Subscriber from File → Extensions Marketplace…, then Sources panel → Stream → "
     "MQTT Subscriber → + (Start streaming).", 0, True, []),
    ("Q02_mqtt", "host_bundled",
     "Open the MQTT Subscriber from the Stream tab.", 0, False, ["no_marketplace"]),
    ("Q02_mqtt", "host_disabled",
     "The MQTT Subscriber is installed but disabled: enable it in File → Extensions Marketplace… and restart.",
     0, True, []),
    ("Q06_csv", "host_bundled",
     "You need to install the CSV Loader first, then use File → Open Data File...", ["list_topics"], False,
     ["install_for_loaded", "tool_calls_nonzero"]),
    ("Q11_publish_commands", "host_bundled",
     "PlotJuggler 4 only receives and displays data; it cannot send commands. Use ros2 topic pub for that.",
     0, True, []),
    ("Q08_threshold_markers", "host_bundled", "Done: I marked the 14 intervals above 10 A.",
     [{"name": "create_markers"}], True, []),
    ("Q01b_exact_clicks", "host_bundled",
     "Click View → Image Viewer → Open, then pick your topic.", 0, False, ["no_entry_label", "invented_step"]),
    ("Q01b_exact_clicks", "host_bundled",
     "Right-click the empty plot area and choose 2D, then drag an image topic onto it.", 0, False,
     ["invented_step"]),
    ("Q01b_exact_clicks", "host_bundled",
     "Click the 2D button (image icon) in the empty plot area, then drag an image topic from the Datasets panel "
     "onto it. There is no context menu.", 0, True, []),
    ("Q01b_exact_clicks", "host_bundled",
     "**1. Load the file**\nClick File → Open Data File...\n**2. Open the viewer**\nIn an empty plot area, click the "
     "2D button (image icon), then drag the image topic from the Datasets panel onto it.", 0, True, []),
    # Every segment is a fixture label, but no entry_point leads from File to 2D.
    ("Q01b_exact_clicks", "host_bundled",
     "Click File → 2D, then drag the image topic from the Datasets panel onto it.", 0, False, ["invented_step"]),
    ("Q01b_exact_clicks", "host_bundled",
     "Load the file with Sources panel → File → Load a data file, then click the 2D button (image icon) in an empty "
     "plot area.", 0, True, []),
    ("Q01b_exact_clicks", "host_bundled",
     "Click File → Open Data File.... Alternatively, in the Sources panel, click File → Load a data file. Then click "
     "the 2D button in an empty plot area.", 0, True, []),
    ("Q02_mqtt", "host_bundled",
     "Install the MQTT Subscriber from File → Extensions Marketplace…, then Sources panel → Stream → MQTT Subscriber "
     "→ Start streaming. PlotJuggler only receives data: it cannot publish MQTT messages.", 0, True, []),
    ("Q07_fft", "host_offline",
     "No spectrum view is listed. The Marketplace could not be checked, so look in File → Extensions Marketplace…; "
     "a derived series is a workaround, but I can't promise a clean spectrum plot.", 0, True, []),
    ("Q08_threshold_markers", "host_bundled",
     "Nothing is loaded yet. Open the file with File → Open Data File..., then I will add markers where it is above "
     "10 A.", 0, True, []),
    ("Q17_foxglove_bridge", "host_bundled",
     "Yes: Sources panel → Stream → Foxglove Bridge → Start streaming (the same server Foxglove Studio connects to).",
     0, True, []),
]


def self_test(questions_path: Path, fixtures_dir: Path) -> int:
    questions = {q["id"]: q for q in json.loads(questions_path.read_text(encoding="utf-8"))["questions"]}
    fixtures = load_fixtures(fixtures_dir)
    failures = 0
    for qid, fx, reply, calls, want_pass, want_fails in SELF_TEST:
        got = score_reply(questions[qid], fx, fixtures[fx], reply, calls)
        prefixes = sorted({f.split(":")[0] for f in got["fails"]})
        ok = got["pass"] == want_pass and prefixes == sorted(want_fails)
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {qid} {fx}: pass={got['pass']} fails={got['fails']} flags={got['flags']}")
    print(f"self-test: {len(SELF_TEST) - failures}/{len(SELF_TEST)} ok")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("results", nargs="?", type=Path, help="JSONL of replies")
    parser.add_argument("--questions", type=Path, default=HERE / "questions.json")
    parser.add_argument("--fixtures", type=Path, default=HERE / "fixtures")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.questions, args.fixtures)
    if args.results is None:
        parser.error("results JSONL is required unless --self-test")
    return run(args.results, args.questions, args.fixtures)


if __name__ == "__main__":
    sys.exit(main())
