// Run with: dotnet run --project <this folder or solution>
// or just call from your own Main.

using CNET.Cce.Examples;

Console.WriteLine("=== Running Perceptual Training Demo ===");
PerceptualTraining.Run();

Console.WriteLine("\n=== Running Milestone 2 Agent Loop Demo ===");
AgentLoop.RunDemo("soul.cnb", "Use calculator and then a certified CNET skill for the task");
