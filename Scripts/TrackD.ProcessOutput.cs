// Copyright (c) 2026 Giovanni Muzio (Kesson)
// SPDX-License-Identifier: MIT
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

// Drain independently of PowerShell's event pump, with bounded buffering and live log files.
public static class TrackDProcessOutput
{
    public static async Task Drain(StreamReader reader, string path, CancellationToken cancellation)
    {
        using var file = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite);
        // Preserve native bytes: child tools may use UTF-8, UTF-16 or the console code page.
        var buffer = new byte[4096];
        int count;
        while ((count = await reader.BaseStream.ReadAsync(buffer.AsMemory(), cancellation).ConfigureAwait(false)) != 0)
        {
            await file.WriteAsync(buffer.AsMemory(0, count), cancellation).ConfigureAwait(false);
            await file.FlushAsync(cancellation).ConfigureAwait(false);
        }
    }
}
