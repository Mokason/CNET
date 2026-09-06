using CNET.Llm.HuggingFace;
using CNET.Llm.Server.Models;

namespace CNET.Llm.Server.Endpoints;

/// <summary>
/// GET /v1/models/available — list locally downloaded models.
/// POST /v1/models/load — hot-swap the loaded model.
/// </summary>
public static class ModelManagementEndpoint
{
    public static void Map(WebApplication app)
    {
        app.MapGet("/v1/models/available", () =>
        {
            var models = HuggingFaceDownloader.ListLocalModels();
            return new AvailableModelsResponse
            {
                Models = models.Select(m => new AvailableModelDto
                {
                    RepoId = m.RepoId,
                    Filename = m.Filename,
                    FullPath = m.FullPath,
                    SizeBytes = m.SizeBytes,
                }).ToArray(),
            };
        });

        app.MapPost("/v1/models/load", async (ModelLoadRequest request, ServerState state, CancellationToken ct) =>
        {
            if (state.RetirementCleanupFailed)
                return Results.Conflict(new ErrorResponse { Error = "retirement_cleanup_failed_restart_required" });
            var resolvedPath = ServerStartup.ResolveModelPath(request.Model, request.Quant);
            if (resolvedPath is null)
                return Results.BadRequest(new ErrorResponse { Error = "Model not found" });

            try
            {
                await state.SwapModelAsync(async token =>
                {
                    var newOptions = state.Options with
                    {
                        Model = request.Model,
                        Quant = request.Quant,
                        Device = request.Device ?? state.Options.Device,
                        GpuLayers = request.GpuLayers ?? state.Options.GpuLayers,
                        CacheTypeK = request.CacheTypeK ?? state.Options.CacheTypeK,
                        CacheTypeV = request.CacheTypeV ?? state.Options.CacheTypeV,
                        Threads = request.Threads ?? state.Options.Threads,
                        DecodeThreads = request.DecodeThreads ?? state.Options.DecodeThreads,
                        SpeculativeModel = request.SpeculativeModel,
                        SpeculativeCandidates = request.SpeculativeK ?? state.Options.SpeculativeCandidates,
                        ModelId = Path.GetFileNameWithoutExtension(resolvedPath),
                    };
                    return await Task.Run(() => ServerStartup.LoadModel(resolvedPath, newOptions), token);
                }, ct);

                return Results.Ok(new ModelLoadResponse
                {
                    Status = state.RetirementCleanupFailed ? "activated_with_retirement_error_restart_required" : "loaded",
                    Model = request.Model,
                });
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested) { throw; }
            catch (Exception)
            {
                return Results.BadRequest(new ErrorResponse { Error = "Model load failed" });
            }
        });
    }
}
