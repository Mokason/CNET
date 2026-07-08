using System;
using System.Runtime.InteropServices;

namespace CnetMcpServer
{
    public static class NarrativeCoherenceTools
    {
        [DllImport("cnet", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool cnet_narrative_should_prefer_specialist(IntPtr task_type);

        public static bool ShouldPreferNarrativeSpecialist(string taskType)
        {
            if (string.IsNullOrEmpty(taskType)) return false;

            IntPtr ptr = Marshal.StringToHGlobalAnsi(taskType);
            bool result = cnet_narrative_should_prefer_specialist(ptr);
            Marshal.FreeHGlobal(ptr);
            return result;
        }
    }
}