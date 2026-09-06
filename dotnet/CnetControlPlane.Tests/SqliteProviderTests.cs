using Microsoft.Data.Sqlite;
using Xunit.Abstractions;

namespace CnetControlPlane.Tests;

public class SqliteProviderTests(ITestOutputHelper output)
{
    [Fact]
    public void Loaded_native_library_meets_security_floor()
    {
        using var connection = new SqliteConnection("Data Source=:memory:;Pooling=False");
        connection.Open();
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT sqlite_version()";
        var loadedVersion = (string)command.ExecuteScalar()!;
        output.WriteLine($"SQLITE_NATIVE_VERSION={loadedVersion}");
        Assert.True(Version.Parse(loadedVersion) >= new Version(3, 50, 2),
            $"SQLITE_NATIVE_SECURITY_RED: loaded {loadedVersion}; CVE-2025-6965 requires >= 3.50.2");
    }

    [Fact]
    public void Loaded_native_library_supports_activation_json_queries()
    {
        using var connection = new SqliteConnection("Data Source=:memory:;Pooling=False");
        connection.Open();
        using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT json_extract($meta, '$.actionable'),
                   json_extract(json_set($meta, '$.actionable', json('false')), '$.actionable'),
                   json_extract($meta, '$.candidate_sha256')
            """;
        command.Parameters.AddWithValue("$meta", """{"actionable":true,"candidate_sha256":"fixture"}""");
        using var reader = command.ExecuteReader();
        Assert.True(reader.Read());
        Assert.Equal(1L, reader.GetInt64(0));
        Assert.Equal(0L, reader.GetInt64(1));
        Assert.Equal("fixture", reader.GetString(2));
        Assert.False(reader.Read());
    }
}
