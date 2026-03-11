"""
Tritium-OS Test Reporting
==========================
SQLite-backed test result storage with statistical metrics and
Tritium-themed HTML report generation.
"""

import json
import math
import sqlite3
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional


class _SafeEncoder(json.JSONEncoder):
    """Handle numpy/non-standard types gracefully."""
    def default(self, obj):
        # numpy bool_, int64, float64, etc.
        if hasattr(obj, 'item'):
            return obj.item()
        if isinstance(obj, (set, frozenset)):
            return list(obj)
        if isinstance(obj, bytes):
            return obj.decode('utf-8', errors='replace')
        return super().default(obj)


@dataclass
class TestResult:
    """Single test result with metadata."""
    category: str
    name: str
    passed: bool
    detail: str = ""
    timestamp: float = 0.0
    duration_ms: float = 0.0
    widget_count: int = 0
    screenshot_path: str = ""
    visual_diff_pct: float = 0.0
    positions_moved: int = 0
    run_number: int = 0

    def __post_init__(self):
        if self.timestamp == 0.0:
            self.timestamp = time.time()


@dataclass
class SoakStats:
    """Statistical summary for soak testing."""
    total_runs: int = 0
    total_tests: int = 0
    total_passed: int = 0
    total_failed: int = 0
    pass_rate: float = 0.0
    error_counts: dict = field(default_factory=dict)
    latencies_ms: list = field(default_factory=list)
    memory_samples: list = field(default_factory=list)

    @property
    def fail_rate(self) -> float:
        return 1.0 - self.pass_rate if self.total_tests > 0 else 0.0

    def latency_p50(self) -> float:
        if not self.latencies_ms:
            return 0.0
        s = sorted(self.latencies_ms)
        return s[len(s) // 2]

    def latency_p95(self) -> float:
        if not self.latencies_ms:
            return 0.0
        s = sorted(self.latencies_ms)
        return s[min(int(len(s) * 0.95), len(s) - 1)]

    def latency_p99(self) -> float:
        if not self.latencies_ms:
            return 0.0
        s = sorted(self.latencies_ms)
        return s[min(int(len(s) * 0.99), len(s) - 1)]

    def memory_trend(self) -> Optional[float]:
        """Returns bytes/second of heap change (negative = leak)."""
        if len(self.memory_samples) < 2:
            return None
        first = self.memory_samples[0]
        last = self.memory_samples[-1]
        dt = last[0] - first[0]
        if dt <= 0:
            return None
        return (last[1] - first[1]) / dt

    def confidence_interval_95(self) -> tuple[float, float]:
        """95% confidence interval for the pass rate (Wilson score)."""
        n = self.total_tests
        if n == 0:
            return (0.0, 1.0)
        p = self.pass_rate
        z = 1.96
        denom = 1 + z * z / n
        center = (p + z * z / (2 * n)) / denom
        spread = z * math.sqrt((p * (1 - p) + z * z / (4 * n)) / n) / denom
        return (max(0.0, center - spread), min(1.0, center + spread))


class TestReport:
    """SQLite-backed test report with HTML generation."""

    def __init__(self, output_dir: str):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.results: list[TestResult] = []
        self.screenshots: list[str] = []
        self.warnings: list[str] = []
        self.start_time = time.time()
        self.soak = SoakStats()
        self._run_number = 0

        # Initialize SQLite database
        self.db_path = self.output_dir / "test_results.db"
        self._init_db()

    def _init_db(self):
        conn = sqlite3.connect(str(self.db_path))
        c = conn.cursor()
        c.execute("""CREATE TABLE IF NOT EXISTS test_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            start_time REAL,
            end_time REAL,
            duration_s REAL,
            total_tests INTEGER,
            passed INTEGER,
            failed INTEGER,
            pass_rate REAL,
            ci_low REAL,
            ci_high REAL,
            soak_runs INTEGER,
            device_ip TEXT,
            device_board TEXT
        )""")
        c.execute("""CREATE TABLE IF NOT EXISTS test_results (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id INTEGER,
            run_number INTEGER,
            category TEXT,
            name TEXT,
            passed INTEGER,
            detail TEXT,
            timestamp REAL,
            duration_ms REAL,
            widget_count INTEGER,
            screenshot_path TEXT,
            visual_diff_pct REAL,
            positions_moved INTEGER,
            FOREIGN KEY (run_id) REFERENCES test_runs(id)
        )""")
        c.execute("""CREATE TABLE IF NOT EXISTS memory_samples (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id INTEGER,
            timestamp REAL,
            heap_free INTEGER,
            psram_free INTEGER,
            FOREIGN KEY (run_id) REFERENCES test_runs(id)
        )""")
        c.execute("""CREATE TABLE IF NOT EXISTS warnings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id INTEGER,
            message TEXT,
            timestamp REAL,
            FOREIGN KEY (run_id) REFERENCES test_runs(id)
        )""")
        c.execute("""CREATE TABLE IF NOT EXISTS latencies (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id INTEGER,
            category TEXT,
            name TEXT,
            latency_ms REAL,
            timestamp REAL,
            FOREIGN KEY (run_id) REFERENCES test_runs(id)
        )""")
        c.execute("""CREATE TABLE IF NOT EXISTS element_coverage (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id INTEGER,
            run_number INTEGER,
            app_name TEXT,
            element_type TEXT,
            element_id TEXT,
            element_text TEXT,
            action TEXT,
            result TEXT,
            passed INTEGER,
            detail TEXT,
            timestamp REAL,
            FOREIGN KEY (run_id) REFERENCES test_runs(id)
        )""")
        conn.commit()
        conn.close()
        self._run_id = None

    def start_run(self, device_ip: str = "", device_board: str = ""):
        conn = sqlite3.connect(str(self.db_path))
        c = conn.cursor()
        c.execute("INSERT INTO test_runs (start_time, device_ip, device_board) VALUES (?, ?, ?)",
                  (time.time(), device_ip, device_board))
        self._run_id = c.lastrowid
        conn.commit()
        conn.close()

    def set_run_number(self, n: int):
        self._run_number = n

    def add(self, category: str, name: str, passed: bool, detail: str = "",
            widgets: Optional[list] = None, duration_ms: float = 0,
            visual_diff_pct: float = 0.0, positions_moved: int = 0,
            screenshot_path: str = "") -> TestResult:
        result = TestResult(
            category=category,
            name=name,
            passed=passed,
            detail=detail,
            duration_ms=duration_ms,
            widget_count=len(widgets) if widgets else 0,
            visual_diff_pct=visual_diff_pct,
            positions_moved=positions_moved,
            screenshot_path=screenshot_path,
            run_number=self._run_number,
        )
        self.results.append(result)

        self.soak.total_tests += 1
        if passed:
            self.soak.total_passed += 1
        else:
            self.soak.total_failed += 1
            key = f"{category}/{name}"
            self.soak.error_counts[key] = self.soak.error_counts.get(key, 0) + 1
        self.soak.pass_rate = self.soak.total_passed / self.soak.total_tests

        if duration_ms > 0:
            self.soak.latencies_ms.append(duration_ms)

        # Write to SQLite
        if self._run_id:
            conn = sqlite3.connect(str(self.db_path))
            c = conn.cursor()
            c.execute("""INSERT INTO test_results
                (run_id, run_number, category, name, passed, detail, timestamp,
                 duration_ms, widget_count, screenshot_path, visual_diff_pct, positions_moved)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (self._run_id, self._run_number, category, name, int(passed), detail,
                 result.timestamp, duration_ms, result.widget_count,
                 screenshot_path, visual_diff_pct, positions_moved))
            if duration_ms > 0:
                c.execute("""INSERT INTO latencies
                    (run_id, category, name, latency_ms, timestamp)
                    VALUES (?, ?, ?, ?, ?)""",
                    (self._run_id, category, name, duration_ms, result.timestamp))
            conn.commit()
            conn.close()

        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] {category}/{name}: {detail}")
        return result

    def add_element(self, app_name: str, element_type: str, element_id: str,
                    element_text: str, action: str, result: str, passed: bool,
                    detail: str = ""):
        """Record an individual UI element interaction for coverage tracking."""
        if self._run_id:
            conn = sqlite3.connect(str(self.db_path))
            c = conn.cursor()
            c.execute("""INSERT INTO element_coverage
                (run_id, run_number, app_name, element_type, element_id,
                 element_text, action, result, passed, detail, timestamp)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (self._run_id, self._run_number, app_name, element_type,
                 element_id, element_text, action, result, int(passed),
                 detail, time.time()))
            conn.commit()
            conn.close()

    def add_warning(self, msg: str):
        self.warnings.append(msg)
        print(f"  [WARN] {msg}")
        if self._run_id:
            conn = sqlite3.connect(str(self.db_path))
            c = conn.cursor()
            c.execute("INSERT INTO warnings (run_id, message, timestamp) VALUES (?, ?, ?)",
                      (self._run_id, msg, time.time()))
            conn.commit()
            conn.close()

    def save_screenshot(self, name: str, data: bytes):
        if not data:
            return ""
        fname = f"{name}.bmp"
        path = self.output_dir / fname
        path.write_bytes(data)
        self.screenshots.append(str(path))
        return str(path)

    def record_memory(self, heap_free: int, psram_free: int):
        self.soak.memory_samples.append((time.time(), heap_free, psram_free))
        if self._run_id:
            conn = sqlite3.connect(str(self.db_path))
            c = conn.cursor()
            c.execute("INSERT INTO memory_samples (run_id, timestamp, heap_free, psram_free) VALUES (?, ?, ?, ?)",
                      (self._run_id, time.time(), heap_free, psram_free))
            conn.commit()
            conn.close()

    def finalize_run(self):
        """Update the test_runs record with final stats."""
        if not self._run_id:
            return
        elapsed = time.time() - self.start_time
        ci_low, ci_high = self.soak.confidence_interval_95()
        conn = sqlite3.connect(str(self.db_path))
        c = conn.cursor()
        c.execute("""UPDATE test_runs SET
            end_time=?, duration_s=?, total_tests=?, passed=?, failed=?,
            pass_rate=?, ci_low=?, ci_high=?, soak_runs=?
            WHERE id=?""",
            (time.time(), elapsed, self.soak.total_tests,
             self.soak.total_passed, self.soak.total_failed,
             self.soak.pass_rate, ci_low, ci_high,
             self.soak.total_runs, self._run_id))
        conn.commit()
        conn.close()

    def generate(self) -> str:
        """Generate text report, JSON, and HTML. Returns text report."""
        self.finalize_run()
        elapsed = time.time() - self.start_time
        total = len(self.results)
        passed = sum(1 for r in self.results if r.passed)
        failed = total - passed
        mem_trend = self.soak.memory_trend()

        # Text report
        text = self._generate_text(elapsed, total, passed, failed, mem_trend)
        (self.output_dir / "report.txt").write_text(text)

        # JSON report
        self._generate_json(elapsed, total, passed, failed, mem_trend)

        # HTML report
        html = self._generate_html(elapsed, total, passed, failed, mem_trend)
        (self.output_dir / "report.html").write_text(html)

        print(f"\n  Reports saved to {self.output_dir}/")
        print(f"  - report.html (visual report)")
        print(f"  - test_results.db (SQLite database)")
        print(f"  - report.txt / results.json")

        return text

    def _generate_text(self, elapsed, total, passed, failed, mem_trend) -> str:
        lines = []
        lines.append("=" * 72)
        lines.append("TRITIUM-OS STABILITY REPORT")
        lines.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append(f"Duration: {elapsed:.1f}s ({elapsed/3600:.1f}h)")
        lines.append(f"Tests: {total} total, {passed} passed, {failed} failed")
        lines.append(f"Pass rate: {self.soak.pass_rate*100:.1f}%")
        ci_low, ci_high = self.soak.confidence_interval_95()
        lines.append(f"95% CI: [{ci_low*100:.1f}%, {ci_high*100:.1f}%]")
        if self.soak.latencies_ms:
            lines.append(f"Latency p50/p95/p99: "
                         f"{self.soak.latency_p50():.0f}/"
                         f"{self.soak.latency_p95():.0f}/"
                         f"{self.soak.latency_p99():.0f} ms")
        if mem_trend is not None:
            trend_str = f"{mem_trend:+.0f} bytes/sec"
            if mem_trend < -100:
                trend_str += " (POSSIBLE LEAK)"
            lines.append(f"Memory trend: {trend_str}")
        lines.append(f"Soak runs: {self.soak.total_runs}")
        lines.append(f"SQLite DB: {self.db_path}")
        lines.append("=" * 72)
        lines.append("")

        categories = {}
        for r in self.results:
            categories.setdefault(r.category, []).append(r)

        for cat, tests in categories.items():
            cat_passed = sum(1 for t in tests if t.passed)
            lines.append(f"## {cat} ({cat_passed}/{len(tests)})")
            for t in tests:
                status = "PASS" if t.passed else "FAIL"
                detail = f" — {t.detail}" if t.detail else ""
                extras = []
                if t.widget_count:
                    extras.append(f"{t.widget_count} widgets")
                if t.visual_diff_pct > 0:
                    extras.append(f"diff={t.visual_diff_pct:.2f}%")
                if t.positions_moved > 0:
                    extras.append(f"{t.positions_moved} moved")
                if t.duration_ms > 0:
                    extras.append(f"{t.duration_ms:.0f}ms")
                extra_str = f" [{', '.join(extras)}]" if extras else ""
                lines.append(f"  [{status}] {t.name}{detail}{extra_str}")
            lines.append("")

        if self.soak.error_counts:
            lines.append("## FAILURE DISTRIBUTION")
            for key, count in sorted(self.soak.error_counts.items(), key=lambda x: -x[1]):
                lines.append(f"  {count:>4}x  {key}")
            lines.append("")

        if self.warnings:
            lines.append("## WARNINGS")
            for w in self.warnings:
                lines.append(f"  - {w}")
            lines.append("")

        return "\n".join(lines)

    def _generate_json(self, elapsed, total, passed, failed, mem_trend):
        ci_low, ci_high = self.soak.confidence_interval_95()
        (self.output_dir / "results.json").write_text(json.dumps({
            "timestamp": datetime.now().isoformat(),
            "duration_s": elapsed,
            "total": total,
            "passed": passed,
            "failed": failed,
            "pass_rate": self.soak.pass_rate,
            "confidence_interval_95": [ci_low, ci_high],
            "latency_p50_ms": self.soak.latency_p50(),
            "latency_p95_ms": self.soak.latency_p95(),
            "latency_p99_ms": self.soak.latency_p99(),
            "memory_trend_bytes_per_sec": mem_trend,
            "soak_runs": self.soak.total_runs,
            "error_distribution": self.soak.error_counts,
            "warnings": self.warnings,
            "db_path": str(self.db_path),
            "results": [
                {
                    "category": str(r.category),
                    "name": str(r.name),
                    "passed": bool(r.passed),
                    "detail": str(r.detail),
                    "timestamp": float(r.timestamp),
                    "duration_ms": float(r.duration_ms),
                    "widget_count": int(r.widget_count),
                    "visual_diff_pct": float(r.visual_diff_pct),
                    "positions_moved": int(r.positions_moved),
                    "screenshot_path": str(r.screenshot_path or ""),
                    "run_number": int(r.run_number),
                }
                for r in self.results
            ],
        }, indent=2, cls=_SafeEncoder))

    def _generate_html(self, elapsed, total, passed, failed, mem_trend) -> str:
        ci_low, ci_high = self.soak.confidence_interval_95()
        pass_pct = self.soak.pass_rate * 100

        # Determine overall status color
        if pass_pct >= 99:
            status_color = "#05ffa1"  # green
            status_text = "EXCELLENT"
        elif pass_pct >= 95:
            status_color = "#00f0ff"  # cyan
            status_text = "GOOD"
        elif pass_pct >= 85:
            status_color = "#fcee0a"  # yellow
            status_text = "DEGRADED"
        else:
            status_color = "#ff2a6d"  # magenta/red
            status_text = "CRITICAL"

        # Memory chart data
        mem_js_data = ""
        if self.soak.memory_samples:
            t0 = self.soak.memory_samples[0][0]
            heap_points = ",".join(f"[{(s[0]-t0)/60:.2f},{s[1]/1024:.0f}]"
                                   for s in self.soak.memory_samples)
            psram_points = ",".join(f"[{(s[0]-t0)/60:.2f},{s[2]/1024:.0f}]"
                                    for s in self.soak.memory_samples)
            mem_js_data = f"var heapData=[{heap_points}];var psramData=[{psram_points}];"
        else:
            mem_js_data = "var heapData=[];var psramData=[];"

        # Per-run pass rate data from SQLite
        run_rate_data = "var runRateData=[];"
        try:
            conn = sqlite3.connect(str(self.db_path))
            rows = conn.execute("""
                SELECT run_number,
                       ROUND(100.0 * SUM(CASE WHEN passed THEN 1 ELSE 0 END) / COUNT(*), 1)
                FROM test_results
                WHERE run_number > 0
                GROUP BY run_number
                ORDER BY run_number
            """).fetchall()
            conn.close()
            if rows:
                pts = ",".join(f"[{r[0]},{r[1]}]" for r in rows)
                run_rate_data = f"var runRateData=[{pts}];"
        except Exception:
            pass

        # Category breakdown
        categories = {}
        for r in self.results:
            categories.setdefault(r.category, []).append(r)

        cat_html = ""
        for cat, tests in categories.items():
            cat_passed = sum(1 for t in tests if t.passed)
            cat_total = len(tests)
            cat_pct = (cat_passed / cat_total * 100) if cat_total else 0
            cat_color = "#05ffa1" if cat_pct >= 99 else "#00f0ff" if cat_pct >= 95 else "#fcee0a" if cat_pct >= 85 else "#ff2a6d"

            rows = ""
            for t in tests:
                icon = "&#x2713;" if t.passed else "&#x2717;"
                row_color = "#05ffa1" if t.passed else "#ff2a6d"
                extras = []
                if t.duration_ms > 0:
                    extras.append(f"{t.duration_ms:.0f}ms")
                if t.widget_count:
                    extras.append(f"{t.widget_count}w")
                if t.visual_diff_pct > 0:
                    extras.append(f"d={t.visual_diff_pct:.1f}%")
                extra = " ".join(extras)
                rows += f'<tr><td style="color:{row_color}">{icon}</td><td>{t.name}</td><td class="detail">{t.detail}</td><td class="mono">{extra}</td></tr>\n'

            cat_html += f"""
            <div class="card">
                <div class="cat-header">
                    <span class="cat-name">{cat}</span>
                    <span class="cat-score" style="color:{cat_color}">{cat_passed}/{cat_total} ({cat_pct:.0f}%)</span>
                </div>
                <table class="results">{rows}</table>
            </div>"""

        # Failure distribution
        fail_html = ""
        if self.soak.error_counts:
            fail_rows = ""
            for key, count in sorted(self.soak.error_counts.items(), key=lambda x: -x[1]):
                fail_rows += f'<tr><td class="mono">{count}</td><td>{key}</td></tr>\n'
            fail_html = f"""
            <div class="card">
                <h2>Failure Distribution</h2>
                <table class="results">{fail_rows}</table>
            </div>"""

        # Warnings
        warn_html = ""
        if self.warnings:
            warn_items = "".join(f"<li>{w}</li>" for w in self.warnings)
            warn_html = f"""
            <div class="card warn-card">
                <h2>Warnings ({len(self.warnings)})</h2>
                <ul>{warn_items}</ul>
            </div>"""

        # Element coverage from SQLite
        coverage_html = self._element_coverage_html()

        # Duration formatting
        hours = int(elapsed // 3600)
        minutes = int((elapsed % 3600) // 60)
        seconds = int(elapsed % 60)
        dur_str = f"{hours}h {minutes}m {seconds}s" if hours > 0 else f"{minutes}m {seconds}s"

        html = f"""<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<title>Tritium-OS Stability Report</title>
<style>
*{{margin:0;padding:0;box-sizing:border-box}}
body{{background:#0a0a0f;color:#c8c8d0;font-family:'Courier New',monospace;padding:20px;max-width:1200px;margin:0 auto}}
h1{{color:#00f0ff;font-size:28px;text-shadow:0 0 20px #00f0ff44;margin-bottom:4px}}
h2{{color:#00f0ff;font-size:16px;margin:12px 0 8px;border-bottom:1px solid #00f0ff22;padding-bottom:4px}}
.subtitle{{color:#666;font-size:13px;margin-bottom:20px}}
.hero{{text-align:center;padding:30px 0;border-bottom:2px solid #00f0ff22;margin-bottom:20px}}
.status-badge{{display:inline-block;font-size:48px;font-weight:bold;padding:10px 30px;
  border:3px solid {status_color};color:{status_color};border-radius:8px;
  text-shadow:0 0 30px {status_color}44;margin:10px 0}}
.pass-rate{{font-size:72px;font-weight:bold;color:{status_color};
  text-shadow:0 0 40px {status_color}44}}
.ci{{color:#666;font-size:14px}}
.grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin:16px 0}}
.stat-box{{background:#0e0e14;border:1px solid #1a1a2e;border-radius:6px;padding:16px;text-align:center}}
.stat-val{{font-size:28px;font-weight:bold;color:#00f0ff}}
.stat-label{{font-size:11px;color:#666;text-transform:uppercase;margin-top:4px}}
.card{{background:#0e0e14;border:1px solid #1a1a2e;border-radius:6px;padding:16px;margin:12px 0}}
.cat-header{{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}}
.cat-name{{color:#00f0ff;font-size:15px;font-weight:bold;text-transform:uppercase}}
.cat-score{{font-weight:bold;font-size:14px}}
table.results{{width:100%;border-collapse:collapse}}
table.results td{{padding:4px 8px;border-bottom:1px solid #12121a;font-size:13px;vertical-align:top}}
table.results td:first-child{{width:20px;text-align:center;font-weight:bold}}
.detail{{color:#888;max-width:400px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}}
.mono{{font-family:'Courier New',monospace;color:#666;font-size:12px}}
.warn-card{{border-color:#fcee0a33}}
.warn-card h2{{color:#fcee0a}}
.warn-card ul{{list-style:none;padding:0}}
.warn-card li{{padding:3px 0;color:#fcee0a99;font-size:13px}}
.warn-card li::before{{content:"! ";color:#fcee0a}}
canvas{{width:100%;height:200px;border:1px solid #1a1a2e;border-radius:4px;margin-top:8px}}
.footer{{text-align:center;color:#333;font-size:11px;margin-top:30px;padding-top:10px;border-top:1px solid #1a1a2e}}
.coverage-grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:6px;margin-top:8px}}
.cov-item{{background:#12121a;border-radius:4px;padding:8px;font-size:12px;text-align:center}}
.cov-item .cov-type{{color:#666;font-size:10px;text-transform:uppercase}}
.cov-item .cov-count{{font-size:18px;font-weight:bold;color:#00f0ff;margin:2px 0}}
</style>
</head><body>
<div class="hero">
    <h1>TRITIUM-OS</h1>
    <div class="subtitle">Stability Test Report &mdash; {datetime.now().strftime('%Y-%m-%d %H:%M')}</div>
    <div class="pass-rate">{pass_pct:.1f}%</div>
    <div class="status-badge">{status_text}</div>
    <div class="ci">95% CI: [{ci_low*100:.1f}%, {ci_high*100:.1f}%]</div>
</div>

<div class="grid">
    <div class="stat-box"><div class="stat-val">{total}</div><div class="stat-label">Total Tests</div></div>
    <div class="stat-box"><div class="stat-val" style="color:#05ffa1">{passed}</div><div class="stat-label">Passed</div></div>
    <div class="stat-box"><div class="stat-val" style="color:#ff2a6d">{failed}</div><div class="stat-label">Failed</div></div>
    <div class="stat-box"><div class="stat-val">{dur_str}</div><div class="stat-label">Duration</div></div>
    <div class="stat-box"><div class="stat-val">{self.soak.total_runs}</div><div class="stat-label">Soak Runs</div></div>
    <div class="stat-box"><div class="stat-val">{self.soak.latency_p50():.0f}ms</div><div class="stat-label">Latency P50</div></div>
    <div class="stat-box"><div class="stat-val">{self.soak.latency_p95():.0f}ms</div><div class="stat-label">Latency P95</div></div>
    <div class="stat-box"><div class="stat-val">{mem_trend if mem_trend is not None else 0:+.0f} B/s</div><div class="stat-label">Memory Trend</div></div>
</div>

{coverage_html}
{cat_html}
{fail_html}
{warn_html}

<div class="card">
    <h2>Pass Rate Per Run</h2>
    <canvas id="rateChart"></canvas>
</div>

<div class="card">
    <h2>Memory Over Time</h2>
    <canvas id="memChart"></canvas>
</div>

<script>
{mem_js_data}
{run_rate_data}
(function(){{
    // Pass rate chart
    var rc=document.getElementById('rateChart');
    if(rc&&runRateData.length>1){{
        var ctx=rc.getContext('2d');
        rc.width=rc.offsetWidth*2;rc.height=300;
        ctx.fillStyle='#0e0e14';ctx.fillRect(0,0,rc.width,rc.height);
        var pad=40;var w=rc.width-pad*2;var h=rc.height-pad*2;
        // Y axis: 70-100%
        var yMin=70,yMax=100;
        // Grid lines
        ctx.strokeStyle='#1a1a2e';ctx.lineWidth=1;
        for(var y=75;y<=100;y+=5){{
            var py=pad+h-(y-yMin)/(yMax-yMin)*h;
            ctx.beginPath();ctx.moveTo(pad,py);ctx.lineTo(pad+w,py);ctx.stroke();
            ctx.fillStyle='#666';ctx.font='10px monospace';ctx.fillText(y+'%',2,py+4);
        }}
        // Plot bars
        var barW=Math.min(20,w/runRateData.length*0.8);
        for(var i=0;i<runRateData.length;i++){{
            var run=runRateData[i][0],rate=runRateData[i][1];
            var x=pad+(i/(runRateData.length-1||1))*w-barW/2;
            var barH=(Math.max(rate,yMin)-yMin)/(yMax-yMin)*h;
            var color=rate>=99?'#05ffa1':rate>=95?'#00f0ff':rate>=85?'#fcee0a':'#ff2a6d';
            ctx.fillStyle=color+'88';ctx.fillRect(x,pad+h-barH,barW,barH);
            ctx.fillStyle=color;ctx.fillRect(x,pad+h-barH,barW,2);
        }}
        ctx.fillStyle='#666';ctx.font='10px monospace';ctx.fillText('Run #',pad+w/2-15,rc.height-5);
    }}

    // Memory chart
    var c=document.getElementById('memChart');
    if(!c||!heapData.length)return;
    var ctx2=c.getContext('2d');
    c.width=c.offsetWidth*2;c.height=400;
    ctx2.fillStyle='#0e0e14';ctx2.fillRect(0,0,c.width,c.height);
    function plot(data,color){{
        if(!data.length)return;
        var xMin=data[0][0],xMax=data[data.length-1][0];
        var yMin=Math.min(...data.map(d=>d[1]))*0.95;
        var yMax=Math.max(...data.map(d=>d[1]))*1.05;
        if(xMax===xMin)xMax=xMin+1;if(yMax===yMin)yMax=yMin+1;
        ctx2.strokeStyle=color;ctx2.lineWidth=2;ctx2.beginPath();
        for(var i=0;i<data.length;i++){{
            var x=(data[i][0]-xMin)/(xMax-xMin)*c.width;
            var y=c.height-(data[i][1]-yMin)/(yMax-yMin)*c.height;
            if(i===0)ctx2.moveTo(x,y);else ctx2.lineTo(x,y);
        }}
        ctx2.stroke();
    }}
    plot(heapData,'#00f0ff');
    plot(psramData,'#05ffa1');
    ctx2.fillStyle='#00f0ff';ctx2.font='11px monospace';ctx2.fillText('Heap (KB)',10,15);
    ctx2.fillStyle='#05ffa1';ctx2.fillText('PSRAM (KB)',10,30);
    ctx2.fillStyle='#666';ctx2.fillText('Time (min)',c.width-80,c.height-5);
}})();
</script>

<div class="footer">
    Tritium-OS &copy; 2026 Valpatel Software LLC &mdash; Generated by Tritium Stability Framework<br>
    SQLite: {self.db_path.name} | {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
</div>
</body></html>"""
        return html

    def _element_coverage_html(self) -> str:
        """Generate element coverage summary from SQLite."""
        if not self._run_id:
            return ""
        try:
            conn = sqlite3.connect(str(self.db_path))
            c = conn.cursor()

            # Per-type summary for current run
            c.execute("""SELECT element_type, COUNT(*), SUM(passed)
                        FROM element_coverage WHERE run_id=?
                        GROUP BY element_type""", (self._run_id,))
            rows = c.fetchall()

            if not rows:
                conn.close()
                return ""

            total_elements = sum(r[1] for r in rows)
            total_passed = sum(r[2] for r in rows)

            items = ""
            for etype, count, passed in rows:
                color = "#05ffa1" if passed == count else "#fcee0a" if passed/count > 0.9 else "#ff2a6d"
                items += f'<div class="cov-item"><div class="cov-type">{etype}</div><div class="cov-count" style="color:{color}">{passed}/{count}</div></div>\n'

            coverage_grid = f"""
            <div class="card">
                <h2>Element Coverage ({total_passed}/{total_elements} passed)</h2>
                <div class="coverage-grid">{items}</div>
            </div>"""

            # Per-element reliability across all soak runs
            c.execute("""SELECT app_name, element_text, element_type,
                                COUNT(*) as total,
                                SUM(CASE WHEN passed THEN 1 ELSE 0 END) as pass_cnt,
                                ROUND(100.0 * SUM(CASE WHEN passed THEN 1 ELSE 0 END) / COUNT(*), 1) as rate
                         FROM element_coverage
                         GROUP BY app_name, element_text
                         ORDER BY rate ASC, app_name""")
            reliability = c.fetchall()
            conn.close()

            if not reliability:
                return coverage_grid

            rel_rows = ""
            for screen, name, etype, total, passed, rate in reliability:
                if rate >= 100:
                    color = "#05ffa1"
                    bar_color = "#05ffa122"
                elif rate >= 90:
                    color = "#00f0ff"
                    bar_color = "#00f0ff22"
                elif rate >= 75:
                    color = "#fcee0a"
                    bar_color = "#fcee0a22"
                else:
                    color = "#ff2a6d"
                    bar_color = "#ff2a6d22"
                rel_rows += (
                    f'<tr style="background:linear-gradient(90deg,{bar_color} {rate}%,transparent {rate}%)">'
                    f'<td style="color:{color}">{rate:.0f}%</td>'
                    f'<td>{screen}</td><td>{name}</td>'
                    f'<td class="mono">{etype}</td>'
                    f'<td class="mono">{passed}/{total}</td></tr>\n'
                )

            perfect = sum(1 for r in reliability if r[5] >= 100)
            reliability_html = f"""
            <div class="card">
                <h2>Element Reliability ({perfect}/{len(reliability)} at 100%)</h2>
                <table class="results">
                <tr style="color:#666;font-size:11px"><td>Rate</td><td>Screen</td><td>Element</td><td>Type</td><td>Pass/Total</td></tr>
                {rel_rows}</table>
            </div>"""

            return coverage_grid + reliability_html
        except Exception:
            return ""
