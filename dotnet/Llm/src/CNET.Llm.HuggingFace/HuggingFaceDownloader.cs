namespace CNET.Llm.HuggingFace;

/// <summary>
/// Downloads files from HuggingFace Hub with progress reporting and resume support via HTTP Range headers.
/// </summary>
public sealed class HuggingFaceDownloader : IDisposable
{
    private const string DefaultCdnBase = "https://huggingface.co";

    /// <summary>Default local model storage directory: <c>~/.cnet-llm/models/</c>.</summary>
    public static string DefaultModelsDirectory =>
        Environment.GetEnvironmentVariable("CNET_LLM_MODELS_DIR")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".cnet-llm", "models");

    private readonly HttpClient _httpClient;
    private readonly bool _ownsClient;

    /// <summary>
    /// Creates a new downloader.
    /// </summary>
    /// <param name="httpClient">Optional pre-configured <see cref="HttpClient"/>.</param>
    /// <param name="token">Optional HuggingFace token. Falls back to <c>HF_TOKEN</c> env var.</param>
    public HuggingFaceDownloader(HttpClient? httpClient = null, string? token = null)
    {
        if (httpClient is not null)
        {
            _httpClient = httpClient;
            _ownsClient = false;
        }
        else
        {
            _httpClient = new HttpClient();
            _ownsClient = true;
        }

        token ??= Environment.GetEnvironmentVariable("HF_TOKEN");
        if (!string.IsNullOrEmpty(token))
            _httpClient.DefaultRequestHeaders.Authorization =
                new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", token);

        _httpClient.DefaultRequestHeaders.UserAgent.ParseAdd("CNET LLM/0.1");
    }

    /// <summary>
    /// Downloads a file from a HuggingFace repository to a local directory.
    /// Supports resuming interrupted downloads via HTTP Range headers.
    /// </summary>
    /// <param name="repoId">Repository ID, e.g. "TheBloke/Llama-2-7B-GGUF".</param>
    /// <param name="filename">Filename within the repo, e.g. "llama-2-7b.Q4_K_M.gguf".</param>
    /// <param name="destinationDir">Target directory. Defaults to <see cref="DefaultModelsDirectory"/>.</param>
    /// <param name="progress">Optional progress callback: (bytesDownloaded, totalBytes). totalBytes may be null if unknown.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>Full path to the downloaded file.</returns>
    public async Task<string> DownloadFileAsync(
        string repoId,
        string filename,
        string? destinationDir = null,
        IProgress<(long bytesDownloaded, long? totalBytes)>? progress = null,
        CancellationToken cancellationToken = default)
    {
        destinationDir ??= DefaultModelsDirectory;

        // Organize by repo: ~/.cnet-llm/models/{owner}/{repo}/
        var repoDir = Path.Combine(destinationDir, repoId.Replace('/', Path.DirectorySeparatorChar));
        Directory.CreateDirectory(repoDir);

        var destPath = Path.Combine(repoDir, filename);
        var partPath = destPath + ".part";

        var url = $"{DefaultCdnBase}/{repoId}/resolve/main/{filename}";

        long existingBytes = 0;
        if (File.Exists(partPath))
            existingBytes = new FileInfo(partPath).Length;

        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        if (existingBytes > 0)
            request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(existingBytes, null);

        using var response = await _httpClient.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);

        // If server doesn't support range or file is complete, start from scratch
        if (existingBytes > 0 && response.StatusCode != System.Net.HttpStatusCode.PartialContent)
            existingBytes = 0;

        response.EnsureSuccessStatusCode();

        long? totalBytes = response.Content.Headers.ContentLength.HasValue
            ? response.Content.Headers.ContentLength.Value + existingBytes
            : null;

        await using var contentStream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
        await using var fileStream = new FileStream(
            partPath,
            existingBytes > 0 ? FileMode.Append : FileMode.Create,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 81920);

        var buffer = new byte[81920];
        long totalRead = existingBytes;
        int bytesRead;

        while ((bytesRead = await contentStream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) > 0)
        {
            await fileStream.WriteAsync(buffer.AsMemory(0, bytesRead), cancellationToken).ConfigureAwait(false);
            totalRead += bytesRead;
            progress?.Report((totalRead, totalBytes));
        }

        // Rename .part to final path once download is complete
        fileStream.Close();
        if (File.Exists(destPath))
            File.Delete(destPath);
        File.Move(partPath, destPath);

        return destPath;
    }

    /// <summary>
    /// Lists locally downloaded models in the models directory.
    /// </summary>
    /// <param name="modelsDir">Models directory. Defaults to <see cref="DefaultModelsDirectory"/>.</param>
    /// <returns>List of local model entries.</returns>
    public static List<LocalModel> ListLocalModels(string? modelsDir = null)
    {
        modelsDir ??= DefaultModelsDirectory;
        var models = new List<LocalModel>();

        if (!Directory.Exists(modelsDir))
            return models;

        // Walk: {modelsDir}/{owner}/{repo}/*.gguf
        foreach (var ownerDir in Directory.GetDirectories(modelsDir))
        {
            var owner = Path.GetFileName(ownerDir);
            foreach (var repoDir in Directory.GetDirectories(ownerDir))
            {
                var repo = Path.GetFileName(repoDir);
                var repoId = $"{owner}/{repo}";
                foreach (var file in Directory.GetFiles(repoDir, "*.gguf"))
                {
                    var info = new FileInfo(file);
                    models.Add(new LocalModel(repoId, info.Name, info.FullName, info.Length, info.LastWriteTimeUtc));
                }
            }
        }

        return models;
    }

    /// <inheritdoc/>
    public void Dispose()
    {
        if (_ownsClient)
            _httpClient.Dispose();
    }
}
