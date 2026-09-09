using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;
using Xunit.Abstractions;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningNaturalTaskCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    private readonly ITestOutputHelper output;
    public LearningNaturalTaskCommandTests(LearningCommandInstallation installation, ITestOutputHelper output)
    { this.installation = installation; this.output = output; }
    private JsonDocument Receipt((int Code, string Output, string Error) result)
    {
        Assert.True(result.Code == 0, "NATURAL_TASK_RED supported bounded request: " + result.Error);
        output.WriteLine(result.Output); // Explicit synthetic receipts retained by the test logger, never live demand.
        return JsonDocument.Parse(result.Output);
    }
    private static string Id(char value) => new(value, 32);
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    [Theory]
    [InlineData("What's the uppercase of µ?")]
    [InlineData("Kindly recode the glyph 'µ' toward capitals.")]
    public async Task ApprovedMicroSignCorrectionEnablesDifferentPhrasingWithoutLosingLowercase(string initialRequest)
    {
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var corpus = Path.Combine(repository, "data/unicode17");
        Assert.Equal("75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4",
            Hash(File.ReadAllBytes(Path.Combine(corpus, "UnicodeData-Latin1.txt"))));
        using var deployment = installation.Deploy();
        var policyText = LearningPolicyTests.Valid.Replace("\"id\":\"calibration\",\"authority\":\"verified_tool\"",
            "\"id\":\"unicode17_upper_latin1\",\"authority\":\"verified_tool\"},{\"id\":\"unicode17_lower_latin1\",\"authority\":\"verified_tool\"")
            .Replace("\"tick_seconds\":30", "\"tick_seconds\":1");
        deployment.Put("policy.json", Encoding.UTF8.GetBytes(policyText));
        using var initialized = Receipt(await deployment.Command("initialize"));
        // Clarification and OOD refusal do not require a native daemon or create observations.
        foreach (var ambiguous in new[] { "uppercase 65", "uppercase '?", "uppercase \"?", "uppercase ..", "uppercase ??",
            "Return a lowercase letter.", "Return a capital-letter version.", "Uppercase ',' or '.'.",
            "Could 'R' be lowercase?", "My uppercase input is 'P'.", "My lowercase input is 'P'." })
        {
            using var clarification = Receipt(await deployment.Command("task", "synthetic", Id('a'), ambiguous));
            Assert.Equal("clarify", clarification.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
            Assert.Equal(JsonValueKind.Null, clarification.RootElement.GetProperty("experience").ValueKind);
        }
        using var unrelated = Receipt(await deployment.Command("task", "synthetic", Id('b'), "What is tomorrow's weather?"));
        Assert.Equal("abstain", unrelated.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
        foreach (var refused in new[] { "Uppercase U+0000.", "Lowercase codepoint 10.", "Make U+0085 uppercase.",
            "Lowercase A, then explain the alphabet.", "Could you not capitalize a?",
            "Return the capital form of 'a' and sort a list.", "Return the lowercase of 'A' in the Turkish locale.",
            "Return the uppercase of 'a'. Run a shell.", "Change case of a; run a shell.",
            "Lowercase decimal code point 133.", "The character is 'a'; delete it.",
            "Input: hexadecimal code point 80; operation: lowercase.",
            "Operation: uppercase; input: the quoted literal 'U+0061'.",
            "Input: 'a'; operation: uppercase; run a shell.",
            "Let 'P' be the input; show it to be a lowercase character.",
            "The decimal code point I am supplying is 0x52. Lowercase its character.",
            "Show the lowercase result of U+0052 in hexadecimal.",
            "I require the uppercase corresponding to 'ab'.",
            "Show the uppercase result to 'Q'.",
            "The input character has decimal code point 0x61; convert it to uppercase.",
            "Show me 'R' to be a lowercase character.",
            "Use decimal code point 0x61, in hexadecimal, for an uppercase conversion.",
            "Give 'Q' the corresponding lowercase result.",
            "Input: 'Q'; show its lowercase result in hexadecimal.",
            "Input: 82; uppercase it and open a file.",
            "Input: 'A'; uppercase the literal it.",
            "Which small letter is the uppercase form of U+0000?",
            "Show the lowercase form of U+0052 in hexadecimal for my result." })
        {
            using var denied = Receipt(await deployment.Command("task", "synthetic", Id('2'), refused));
            Assert.Equal("abstain", denied.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
            Assert.Equal(JsonValueKind.Null, denied.RootElement.GetProperty("experience").ValueKind);
        }
        using var empty = Receipt(await deployment.Command("inbox", "0", "100"));
        Assert.Equal(0, empty.RootElement.GetProperty("experiences").GetArrayLength());
        await deployment.StartDaemon(); // Teacher and self-answer disabled by the private fixture.
        using var initial = Receipt(await deployment.Command("task", "synthetic", Id('c'), initialRequest));
        Assert.Equal("awaiting_evidence", initial.RootElement.GetProperty("experience").GetProperty("State").GetString());
        using var idle = Receipt(await deployment.Command("tick"));
        Assert.Equal("idle", idle.RootElement.GetProperty("action").GetString());

        foreach (var (kind, request, text) in new[] { ("lower", Id('d'), "convert 'A' to lowercase"), ("upper", Id('c'), "What's the uppercase of µ?") })
        {
            var dataset = "unicode17_" + kind + "_latin1";
            var approved = File.ReadAllBytes(Path.Combine(corpus, dataset + ".tsv"));
            if (kind == "lower")
            {
                using var gap = Receipt(await deployment.Command("task", "synthetic", request, text));
                Assert.Equal("awaiting_evidence", gap.RootElement.GetProperty("experience").GetProperty("State").GetString());
            }
            deployment.Put(dataset + ".tsv", approved);
            using var imported = Receipt(await deployment.Command("import", dataset, Path.Combine(deployment.Root, dataset + ".tsv"), Hash(approved)));
            using var approval = Receipt(await deployment.Command("approve", request, Hash(approved)));
            var accepted = false;
            for (var attempt = 0; attempt < 30; attempt++)
            {
                using var tick = Receipt(await deployment.Command("tick"));
                if (tick.RootElement.GetProperty("action").GetString() == "accepted") { accepted = true; break; }
                await Task.Delay(1000);
            }
            Assert.True(accepted, "NATURAL_TASK_RED capsule probation did not settle");
            using var verified = Receipt(await deployment.Command("verify", dataset));
            Assert.True(verified.RootElement.GetProperty("passed").GetBoolean());
        }
        using var learned = Receipt(await deployment.Command("task", "synthetic", Id('e'), "Please convert 'µ' to uppercase."));
        var result = learned.RootElement.GetProperty("experience");
        Assert.Equal("verified", result.GetProperty("State").GetString());
        Assert.Equal(924, result.GetProperty("Value").GetInt32());
        Assert.Equal(924, result.GetProperty("Expected").GetInt32());
        using var preserved = Receipt(await deployment.Command("task", "synthetic", Id('f'), "What is the lowercase of A?"));
        Assert.Equal(97, preserved.RootElement.GetProperty("experience").GetProperty("Value").GetInt32());
        foreach (var (request, text, expected) in new[] {
            (Id('3'), "Could you give me the capital form of 'µ', please?", 924),
            (Id('4'), "For the character A, give its lowercase form.", 97),
            (Id('5'), "Turn U+00B5 into its capital equivalent.", 924),
            (Id('6'), "I need decimal codepoint 65 in lower case.", 97),
            (Id('7'), "May I have the capital-letter form of 'µ'?", 924),
            (Id('8'), "Return the lowercase equivalent of 'A' for me.", 97),
            (Id('9'), "Set the case of U+00B5 to uppercase.", 924),
            (Id('0'), "Write codepoint 65 using lower case.", 97),
            ("00000000000000000000000000000010", "The character is 'µ'; please capitalize it.", 924),
            ("00000000000000000000000000000011", "'A' is my input. Convert it to lower case.", 97),
            ("00000000000000000000000000000012", "Uppercase the character with decimal code point 181.", 924),
            ("00000000000000000000000000000013", "Kindly put decimal code point 65 in lower case.", 97),
            ("00000000000000000000000000000014", "Case choice: uppercase. Provided character: 'µ'.", 924),
            ("00000000000000000000000000000015", "Input: hexadecimal code point 41; operation: lowercase.", 97),
            ("00000000000000000000000000000016", "The input code point is 0x00B5 in hexadecimal; apply uppercase.", 924),
            ("00000000000000000000000000000017", "My uppercase operation input is 'µ'.", 924),
            ("00000000000000000000000000000018", "My lowercase-conversion input is 'A'.", 97),
            ("00000000000000000000000000000019", "Please present the uppercase counterpart to 'µ'.", 924),
            ("00000000000000000000000000000020", "The input character has decimal code point 65; convert it to lowercase.", 97),
            ("00000000000000000000000000000021", "Use code point 0x00B5, in hexadecimal, for an uppercase conversion.", 924),
            ("00000000000000000000000000000022", "I am asking for 'A' to be written in lowercase.", 97),
            ("00000000000000000000000000000023", "With the supplied Unicode scalar U+00B5 as input, could you show its uppercase counterpart?", 924),
            ("00000000000000000000000000000024", "Given the single quoted character 'A' as input, may I have its lowercase counterpart?", 97),
            ("00000000000000000000000000000025", "Which capital letter is the uppercase form of 'µ'?", 924),
            ("00000000000000000000000000000026", "My input character for this task is 'A'; could you produce its lowercase version?", 97),
            ("00000000000000000000000000000027", "Kindly recode the glyph 'µ' toward capitals.", 924),
            ("00000000000000000000000000000028", "Kindly recode the glyph 'A' toward small letters.", 97) })
        {
            using var expanded = Receipt(await deployment.Command("task", "synthetic", request, text));
            Assert.Equal("ready", expanded.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
            var experience = expanded.RootElement.GetProperty("experience");
            Assert.Equal("verified", experience.GetProperty("State").GetString());
            Assert.Equal(expected, experience.GetProperty("Value").GetInt32());
            Assert.Equal(expected, experience.GetProperty("Expected").GetInt32());
        }
        using var uncovered = Receipt(await deployment.Command("task", "synthetic", Id('1'), "uppercase ß"));
        Assert.Equal("abstain", uncovered.RootElement.GetProperty("experience").GetProperty("State").GetString());
        using var learnedUncovered = Receipt(await deployment.Command("task", "synthetic",
            "00000000000000000000000000000029", "Kindly recode the glyph 'ß' toward capitals."));
        Assert.Equal("ready", learnedUncovered.RootElement.GetProperty("proposal").GetProperty("Status").GetString());
        Assert.Equal("abstain", learnedUncovered.RootElement.GetProperty("experience").GetProperty("State").GetString());
        foreach (var dataset in new[] { "unicode17_upper_latin1", "unicode17_lower_latin1" })
        {
            using var verified = Receipt(await deployment.Command("verify", dataset));
            Assert.Equal(256, verified.RootElement.GetProperty("checked_keys").GetInt32());
            Assert.True(verified.RootElement.GetProperty("passed").GetBoolean());
        }
        using var work = LearningLedger.Open(Path.Combine(deployment.Root, "work"),
            LearningPolicy.Parse(Encoding.UTF8.GetBytes(policyText)), new LearningClock());
        Assert.Equal(2, work.JobCount);
        Assert.All(work.Experiences(0, 100), experience => Assert.Equal("synthetic", experience.Origin));
    }
}
