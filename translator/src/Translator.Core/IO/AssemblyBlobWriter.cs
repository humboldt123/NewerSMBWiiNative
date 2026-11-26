using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using Translator.Core.Loading;

namespace Translator.Core.IO;

internal sealed record AssemblyBlob(string FileName, string Symbol, ReadOnlyMemory<byte> Data, string Comment);

internal static class AssemblyBlobWriter
{
    public static void Write(
        string assemblyPath,
        string blobDirectory,
        string blobReferenceDirectory,
        IReadOnlyList<AssemblyBlob> blobs,
        params string[] headerLines)
    {
        Directory.CreateDirectory(blobDirectory);
        var expectedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var assembly = new StringBuilder();
        foreach (var header in headerLines) assembly.AppendLine(header);
        // PE/COFF (Windows), ELF (Linux) and Mach-O (macOS) spell a read-only data section
        // differently, and Mach-O prefixes C symbols with an underscore. A .S file goes through the
        // C preprocessor, so one file picks the right spelling for whichever host assembles it.
        assembly.AppendLine("#if defined(__APPLE__)");
        assembly.AppendLine(".section __TEXT,__const");
        assembly.AppendLine("#define BLOB_SYMBOL(name) _##name");
        assembly.AppendLine("#elif defined(_WIN32)");
        assembly.AppendLine(".section .rdata,\"dr\"");
        assembly.AppendLine("#define BLOB_SYMBOL(name) name");
        assembly.AppendLine("#else");
        assembly.AppendLine(".section .rodata,\"a\",@progbits");
        assembly.AppendLine("#define BLOB_SYMBOL(name) name");
        assembly.AppendLine("#endif");
        assembly.AppendLine();

        foreach (var blob in blobs)
        {
            var blobPath = Path.Combine(blobDirectory, blob.FileName);
            expectedFiles.Add(Path.GetFullPath(blobPath));
            FileOutput.WriteBytesIfChanged(blobPath, blob.Data.Span);
            var hash = ChecksumUtilities.Sha256Hex(blob.Data.Span);
            assembly.AppendLine($"// {blob.Comment}; sha256={hash}");
            assembly.AppendLine(".p2align 4");
            assembly.AppendLine($".globl BLOB_SYMBOL({blob.Symbol})");
            assembly.AppendLine($"BLOB_SYMBOL({blob.Symbol}):");
            var referencePath = Path.Combine(blobReferenceDirectory, blob.FileName);
            assembly.AppendLine($".incbin \"{SanitizeAssemblyPath(referencePath)}\"");
            assembly.AppendLine();
        }

        foreach (var stalePath in Directory.EnumerateFiles(blobDirectory, "*.bin"))
        {
            if (!expectedFiles.Contains(Path.GetFullPath(stalePath))) File.Delete(stalePath);
        }
        // ELF only: without a .note.GNU-stack section the linker assumes this object needs an
        // executable stack and warns. It has no code, so say so explicitly.
        assembly.AppendLine("#if !defined(__APPLE__) && !defined(_WIN32)");
        assembly.AppendLine(".section .note.GNU-stack,\"\",@progbits");
        assembly.AppendLine("#endif");
        FileOutput.WriteTextIfChanged(assemblyPath, assembly.ToString());
    }

    private static string SanitizeAssemblyPath(string path) => Path.GetFullPath(path).Replace('\\', '/');
}
