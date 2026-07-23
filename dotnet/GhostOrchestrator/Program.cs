// ghost-orchestrator — the reconcile loop over the ghost memory stack.
//
// Self-governance from state, not notifications: every tick it re-derives the
// world (store growth, inbox age, ledger gap statuses, which lane daemons are
// alive), plans whatever actions close the gap between observed and desired,
// and executes them under discipline — cooldowns, exponential backoff,
// per-tick budgets, a durable decision journal with reasons, and permission
// gates on anything that costs real resources. A missed event costs nothing:
// the next tick reaches the same conclusion from the same facts.
//
//   ghost-orchestrator --once --dry-run                      # see what it would do
//   ghost-orchestrator --inbox <path> --ledger <path>        # govern for real
//   ghost-orchestrator --allow-teacher-start ...             # may start the GPU lane

using System.Diagnostics;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Orchestration;

string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
string store = Path.Combine(home, ".cnet-llm", "ghost", "memory.jsonl");
string? inbox = Environment.GetEnvironmentVariable("CNET_GAP_INBOX");
string? ledger = null;
string stateDir = Path.Combine(home, ".cnet-llm", "ghost");
string drainerSvc = "cnet-personal-ai-lane.service";
string teacherSvc = "cnet-gap-lane.service";
int intervalSec = 60;
int minNewBlobs = 6;
bool once = false, dryRun = false, allowTeacherStart = false;

for (int i = 0; i < args.Length; i++)
{
    string? Next() => i + 1 < args.Length ? args[++i] : null;
    switch (args[i])
    {
        case "--store": store = Next() ?? store; break;
        case "--inbox": inbox = Next(); break;
        case "--ledger": ledger = Next(); break;
        case "--state-dir": stateDir = Next() ?? stateDir; break;
        case "--drainer-service": drainerSvc = Next() ?? drainerSvc; break;
        case "--teacher-service": teacherSvc = Next() ?? teacherSvc; break;
        case "--interval": intervalSec = int.Parse(Next() ?? "60"); break;
        case "--min-new-blobs": minNewBlobs = int.Parse(Next() ?? "6"); break;
        case "--once": once = true; break;
        case "--dry-run": dryRun = true; break;
        case "--allow-teacher-start": allowTeacherStart = true; break;
        default:
            Console.Error.WriteLine(
                "usage: ghost-orchestrator [--store <jsonl>] [--inbox <path>] [--ledger <path>]\n" +
                "       [--state-dir <dir>] [--interval N] [--min-new-blobs N] [--once] [--dry-run]\n" +
                "       [--allow-teacher-start] [--drainer-service <svc>] [--teacher-service <svc>]");
            return 2;
    }
}

// The ledger conventionally sits beside the inbox: <base>.inbox / <base>.gaps.txt.
if (ledger is null && inbox is not null && inbox.EndsWith(".inbox", StringComparison.Ordinal))
{
    string candidate = inbox[..^".inbox".Length] + ".gaps.txt";
    if (File.Exists(candidate)) ledger = candidate;
}

var config = new OrchestratorConfig
{
    MinNewBlobs = minNewBlobs,
    AllowTeacherStart = allowTeacherStart,
};
string statePath = Path.Combine(stateDir, "orchestrator.state.json");
string journalPath = Path.Combine(stateDir, "orchestrator.journal.jsonl");
OrchestratorState state = OrchestratorState.Load(statePath);
var journal = new DecisionJournal(journalPath);

// The surprise scanner needs the window words sidecar (native convention:
// <CNET_WINDOW_FILE minus .txt> + .words.txt).
string? wordsPath = null;
string? windowFile = Environment.GetEnvironmentVariable("CNET_WINDOW_FILE");
if (windowFile is not null && windowFile.EndsWith(".txt", StringComparison.Ordinal))
{
    string candidate = windowFile[..^4] + ".words.txt";
    if (File.Exists(candidate)) wordsPath = candidate;
}

var executors = new Dictionary<string, Func<PlannedAction, OrchestratorState, string>>
{
    ["janitor"] = (_, _) =>
    {
        int archived = BlobStore.Compact(store);
        if (archived < 0) return "store busy (live session holds the lock) — will retry";
        // Rotate the orchestrator's own journal when it outgrows 1 MB. Own
        // file only: rotating another process's append target misdirects it.
        string journalFile = Path.Combine(stateDir, "orchestrator.journal.jsonl");
        string rotated = "";
        var info = new FileInfo(journalFile);
        if (info.Exists && info.Length > 1_000_000)
        {
            File.Move(journalFile, journalFile + ".1", overwrite: true);
            rotated = "; journal rotated";
        }
        return $"archived {archived} dead lines to {store}.archive.jsonl{rotated}";
    },
    ["curiosity"] = (_, s) =>
    {
        if (inbox is null || wordsPath is null)
            throw new InvalidOperationException(
                "curiosity needs --inbox and a window words sidecar (CNET_WINDOW_FILE)");
        string recDir = inbox.EndsWith(".inbox", StringComparison.Ordinal)
            ? inbox[..^".inbox".Length] + ".records" : inbox + ".records";
        GhostSnapshot snap = BlobStore.Snapshot(store);
        var scanner = new SurpriseScanner(wordsPath, recDir);
        var surprises = scanner.Scan(snap, s.LastCuriosityMaxId);
        s.LastCuriosityMaxId = snap.MaxSeenId;
        if (surprises.Count == 0) return "no surprises — taught knowledge holds";

        var rec = scanner.BuildObservationRecord(snap, surprises)!.Value;
        Directory.CreateDirectory(recDir);
        File.WriteAllText(Path.Combine(recDir, $"skill_{rec.Name}.txt"), rec.Record);
        int rc = GhostConsolidator.NoteRecordSkill(inbox, rec.Name, rec.Record);
        string detail = string.Join("; ", surprises.Take(3).Select(x =>
            x.Kind == "contradiction"
                ? $"{x.FromWord}->{x.ObservedWord} contradicts taught {x.FromWord}->{x.TaughtWord}"
                : $"novel {x.FromWord}->{x.ObservedWord} ×{x.Occurrences}"));
        return rc == 0
            ? $"{surprises.Count} surprise(s) [{detail}] -> record {rec.Name} noted"
            : $"{surprises.Count} surprise(s) found but note_skill rc={rc}";
    },
    ["consolidate"] = (_, s) =>
    {
        if (inbox is null)
            throw new InvalidOperationException("no inbox configured (--inbox or CNET_GAP_INBOX)");
        GhostSnapshot snapshot = BlobStore.Snapshot(store);
        var consolidator = new GhostConsolidator(snapshot)
        {
            // Shared taste: same judgment state the TUI learns into.
            Judge = new CNET.Cce.Llm.Judgment.AdaptiveJudge(
                Path.GetDirectoryName(Path.GetFullPath(store))!),
        };
        var items = consolidator.Extract();
        var receipts = consolidator.Emit(items, inbox);
        string recordsDir = inbox.EndsWith(".inbox", StringComparison.Ordinal)
            ? inbox[..^".inbox".Length] + ".records"
            : inbox + ".records";
        var corrections = consolidator.ExtractCorrections();
        receipts.AddRange(consolidator.EmitCorrections(corrections, inbox, recordsDir));
        int ok = receipts.Count(r => r.Emitted);
        if (ok == 0 && receipts.Count > 0)
            throw new InvalidOperationException(receipts[0].Error ?? "all emissions failed");
        s.LastConsolidatedMaxId = snapshot.MaxSeenId;   // watermark even when 0 teachable
        return $"extracted {items.Count}+{corrections.Count} corrections, " +
               $"noted {ok}/{receipts.Count} into {inbox}";
    },
    ["start-teacher-lane"] = (_, _) =>
    {
        RunSystemctl("start", teacherSvc);
        // Verify — a unit that immediately dies must count as failure, not success.
        Thread.Sleep(3000);
        if (!ServiceActive(teacherSvc))
            throw new InvalidOperationException($"{teacherSvc} did not reach active state");
        return $"started {teacherSvc} (verified active)";
    },
};

var reconciler = new Reconciler(config, state, journal, executors);
Console.WriteLine($"ghost-orchestrator | store={store}");
Console.WriteLine($"inbox={inbox ?? "(none)"} ledger={ledger ?? "(none)"} " +
                  $"dry-run={dryRun} allow-teacher-start={allowTeacherStart}");

while (true)
{
    Observations obs = Observe();
    List<Decision> decisions = reconciler.Tick(obs, dryRun);
    state.Save(statePath);

    Console.WriteLine($"[{obs.NowUtc:HH:mm:ss}] blobs={obs.StoreBlobs} maxId={obs.StoreMaxSeenId} " +
        $"inbox={(obs.InboxExists ? obs.InboxLines.ToString() : "-")} " +
        $"ghostGaps={obs.LedgerGhostGaps}({obs.LedgerGhostWaiting} waiting) " +
        $"drainer={(obs.DrainerActive ? "up" : "down")} teacher={(obs.TeacherLaneActive ? "up" : "down")}" +
        (decisions.Count == 0 ? " — steady state" : ""));
    foreach (Decision d in decisions)
        Console.WriteLine($"  {(d.Executed ? "✔" : d.DryRun ? "○" : "•")} {d.Action}: {d.Reason} → {d.Outcome}");

    if (once) break;
    Thread.Sleep(TimeSpan.FromSeconds(intervalSec));
}
return 0;

Observations Observe()
{
    GhostSnapshot snap = BlobStore.Snapshot(store);

    bool inboxExists = inbox is not null && File.Exists(inbox);
    int inboxLines = 0;
    double inboxAge = 0;
    if (inboxExists)
    {
        inboxLines = File.ReadAllLines(inbox!).Count(l => !string.IsNullOrWhiteSpace(l));
        inboxAge = (DateTime.UtcNow - File.GetLastWriteTimeUtc(inbox!)).TotalSeconds;
    }

    int ghostGaps = 0, ghostWaiting = 0;
    if (ledger is not null && File.Exists(ledger))
    {
        foreach (string line in File.ReadLines(ledger))
        {
            if (!line.Contains("skill_gh_", StringComparison.Ordinal)) continue;
            ghostGaps++;
            if (line.Contains("waiting_oracle", StringComparison.Ordinal)) ghostWaiting++;
        }
    }

    return new Observations(
        DateTime.UtcNow, snap.MaxSeenId, snap.All().Count, snap.CorruptLinesSkipped,
        inboxExists, inboxLines, inboxAge, ghostGaps, ghostWaiting,
        ServiceActive(drainerSvc), ServiceActive(teacherSvc),
        snap.DeadLines, snap.TotalLines);
}

static bool ServiceActive(string service)
{
    try
    {
        using var p = Process.Start(new ProcessStartInfo("systemctl", $"--user is-active {service}")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        });
        if (p is null) return false;
        string output = p.StandardOutput.ReadToEnd().Trim();
        p.WaitForExit(5000);
        return output == "active";
    }
    catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
    {
        return false;   // no systemd here — policies degrade to advisories
    }
}

static void RunSystemctl(string verb, string service)
{
    using var p = Process.Start(new ProcessStartInfo("systemctl", $"--user {verb} {service}")
    {
        RedirectStandardOutput = true,
        RedirectStandardError = true,
    }) ?? throw new InvalidOperationException("systemctl unavailable");
    p.WaitForExit(15000);
    if (p.ExitCode != 0)
        throw new InvalidOperationException(
            $"systemctl {verb} {service} rc={p.ExitCode}: {p.StandardError.ReadToEnd().Trim()}");
}
