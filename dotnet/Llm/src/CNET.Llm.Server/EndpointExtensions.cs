using CNET.Llm.Server.Endpoints;

namespace CNET.Llm.Server;

/// <summary>
/// Extension methods for registering all CNET LLM API endpoints.
/// </summary>
public static class EndpointExtensions
{
    /// <summary>
    /// Maps all CNET LLM OpenAI-compatible API endpoints.
    /// </summary>
    /// <param name="app">The web application.</param>
    /// <param name="serveUi">When true, also serves the embedded web chat UI at <c>GET /</c>.</param>
    public static WebApplication MapCnetLlmEndpoints(this WebApplication app, bool serveUi = false)
    {
        app.UseMiddleware<ServerSecurityMiddleware>();
        ChatCompletionEndpoint.Map(app);
        CompletionEndpoint.Map(app);
        ModelEndpoint.Map(app);
        TokenizeEndpoint.Map(app);
        HealthEndpoint.Map(app);
        PropsEndpoint.Map(app);
        ConfigEndpoint.Map(app);
        ModelManagementEndpoint.Map(app);
        ModelInspectEndpoint.Map(app);

        if (serveUi)
            WebUIEndpoint.Map(app);

        return app;
    }
}
