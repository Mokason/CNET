using CnetControlPlane.Acceptance;
using CnetControlPlane.Activate;
using CnetControlPlane.Capability;
using CnetControlPlane.Ds4;
using CnetControlPlane.Hermes;
using CnetControlPlane.Ingest;
using CnetControlPlane.Usage;
using CnetControlPlane.Learning;

static int Usage()
{
    Console.Error.WriteLine("""
        cnet-control <verb> ...

        Verbs:
          ingest-suggestions
          activate-suggestions
          render-hermes-wrapper
          run-hermes-wrapper
          real-model-acceptance
          verify-ds4
          usage-window
          capability-cert
          capability-prereq
          learning
        """);
    return 2;
}

if (args.Length == 0)
    return Usage();

var verb = args[0];
var rest = args.Skip(1).ToArray();
try
{
    return verb switch
    {
        "ingest-suggestions" => SuggestionIngest.RunCli(rest),
        "activate-suggestions" => SuggestionActivation.RunCli(rest),
        "render-hermes-wrapper" => HermesWrapperRenderer.RunCli(rest),
        "run-hermes-wrapper" => HermesWrapperRunner.RunCli(rest),
        "real-model-acceptance" => RealModelAcceptance.RunCli(rest),
        "verify-ds4" => Ds4EndpointVerifier.RunCli(rest),
        "usage-window" => UsageWindow.RunCli(rest),
        "capability-cert" => CapabilityCertRunner.RunCli(rest),
        "capability-prereq" => EvaluatorPrereq.RunCli(rest),
        "learning" => LearningCommand.RunCli(rest),
        "-h" or "--help" or "help" => Usage(),
        _ => Usage(),
    };
}
catch (Exception ex) when (ex is ArgumentException or InvalidOperationException)
{
    Console.Error.WriteLine(ex.Message);
    return 2;
}
