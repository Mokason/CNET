using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Orchestration;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Rung 3: prediction-error-driven curriculum. Contradictions (taught A→B,
/// observed A→C) surface immediately; novelty needs recurrence across blobs;
/// old blobs behind the watermark are invisible; the observation record
/// carries the verbatim evidence.
/// </summary>
public sealed class SurpriseScannerTests : IDisposable
{
    private readonly string _dir;

    public SurpriseScannerTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-surprise", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
        Directory.CreateDirectory(RecordsDir);
        // 8-word window, sidecar format.
        File.WriteAllText(WordsPath,
            "10\tthe\n11\tport\n12\tis\n13\tnine\n14\teight\n15\topen\n16\tclosed\n17\tnow\n");
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string WordsPath => Path.Combine(_dir, "w.words.txt");
    private string RecordsDir => Path.Combine(_dir, "records");
    private string StorePath() => Path.Combine(_dir, "s.jsonl");

    [Fact]
    public void Contradiction_SurfacesFromASingleObservation()
    {
        // Taught: "port is nine" (is -> nine).
        File.WriteAllText(Path.Combine(RecordsDir, "skill_corr_aaaa.txt"),
            "the port is nine");
        using var store = BlobStore.Open(StorePath());
        // Observed: "the port is eight" (is -> eight): live disagreement.
        store.Append("s1", 0, "user", "the port is eight", 5);

        var scanner = new SurpriseScanner(WordsPath, RecordsDir);
        var surprises = scanner.Scan(store, sinceBlobId: 0);

        var s = Assert.Single(surprises, x => x.Kind == "contradiction");
        Assert.Equal("is", s.FromWord);
        Assert.Equal("nine", s.TaughtWord);
        Assert.Equal("eight", s.ObservedWord);
        Assert.Equal(1, s.Occurrences);      // one observation suffices
    }

    [Fact]
    public void Novelty_NeedsRecurrence_AcrossDistinctBlobs()
    {
        using var store = BlobStore.Open(StorePath());          // nothing taught
        store.Append("s1", 0, "user", "port open now", 4);      // port->open x1
        store.Append("s1", 1, "user", "the port open again", 5);
        store.Append("s2", 0, "user", "keep that port open", 5);

        var scanner = new SurpriseScanner(WordsPath, RecordsDir);
        var surprises = scanner.Scan(store, 0);

        var s = Assert.Single(surprises, x => x.Kind == "novelty" &&
                                              x.FromWord == "port");
        Assert.Equal("open", s.ObservedWord);
        Assert.Equal(3, s.Occurrences);
        Assert.True(s.EvidenceBlobIds.Count >= 2);

        // A transition seen once ("open now" appears in only one blob... it
        // recurs? no: open->now occurred once) never becomes curriculum.
        Assert.DoesNotContain(surprises, x => x.FromWord == "open" && x.ObservedWord == "now");
    }

    [Fact]
    public void Watermark_HidesAlreadyScannedBlobs()
    {
        File.WriteAllText(Path.Combine(RecordsDir, "skill_corr_aaaa.txt"),
            "the port is nine");
        using var store = BlobStore.Open(StorePath());
        long old = store.Append("s1", 0, "user", "the port is eight", 5).Id;

        var scanner = new SurpriseScanner(WordsPath, RecordsDir);
        Assert.Empty(scanner.Scan(store, sinceBlobId: old));    // behind watermark
        Assert.Single(scanner.Scan(store, sinceBlobId: old - 1));
    }

    [Fact]
    public void ObservationRecord_CarriesVerbatimEvidence_DeterministicName()
    {
        File.WriteAllText(Path.Combine(RecordsDir, "skill_corr_aaaa.txt"),
            "the port is nine");
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "the port is eight now", 6);

        var scanner = new SurpriseScanner(WordsPath, RecordsDir);
        var surprises = scanner.Scan(store, 0);
        var rec1 = scanner.BuildObservationRecord(store, surprises);
        var rec2 = scanner.BuildObservationRecord(store, surprises);

        Assert.NotNull(rec1);
        Assert.StartsWith("obs_", rec1!.Value.Name);
        Assert.Contains("the port is eight now", rec1.Value.Record);   // verbatim
        Assert.StartsWith("#v2\n", rec1.Value.Record);
        Assert.Equal(rec1.Value.Name, rec2!.Value.Name);               // coalesces

        Assert.Null(scanner.BuildObservationRecord(store, []));
    }

    [Fact]
    public void TaughtCorpus_ReadsAllRecordFamilies()
    {
        File.WriteAllText(Path.Combine(RecordsDir, "skill_vrf_bbbb.txt"),
            "the port is nine");
        using var store = BlobStore.Open(StorePath());
        store.Append("s1", 0, "user", "the port is eight", 5);

        var surprises = new SurpriseScanner(WordsPath, RecordsDir).Scan(store, 0);
        Assert.Contains(surprises, s => s.Kind == "contradiction");   // vrf records teach too
    }
}
