// Read-only extraction frontend for the bundled LGPL-3.0 LibOrbisPkg library.
// Unlike PkgTool's default memory map, this never requests write access to a PKG.
using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading.Tasks;
using LibOrbisPkg.PFS;
using LibOrbisPkg.PKG;
using LibOrbisPkg.Util;

internal static class PcvrPkgExtract {
    public static int Main(string[] args) {
        if (args.Length != 3) {
            Console.Error.WriteLine("Usage: PcvrPkgExtract input.pkg output_directory passcode");
            return 2;
        }
        try {
            using (var file = new FileStream(args[0], FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var map = MemoryMappedFile.CreateFromFile(file, null, 0,
                MemoryMappedFileAccess.Read, HandleInheritability.None, false)) {
                Pkg pkg;
                using (var view = map.CreateViewStream(0, 0, MemoryMappedFileAccess.Read)) {
                    pkg = new PkgReader(view).ReadPkg();
                }
                var key = Crypto.ComputeKeys(pkg.Header.content_id, args[2], 1);
                using (var view = map.CreateViewAccessor((long)pkg.Header.pfs_image_offset,
                    (long)pkg.Header.pfs_image_size, MemoryMappedFileAccess.Read)) {
                    var outer = new PfsReader(view, pkg.Header.pfs_flags, key);
                    var inner = new PfsReader(new PFSCReader(outer.GetFile("pfs_image.dat").GetView()));
                    var root = Path.GetFullPath(args[1]).TrimEnd('\\') + "\\";
                    Parallel.ForEach(inner.GetAllFiles(), () => new byte[65536], (entry, state, buffer) => {
                        var relative = entry.FullName.Replace('/', '\\').TrimStart('\\');
                        foreach (var part in relative.Split('\\')) {
                            if (part == "." || part == ".." || part.IndexOf(':') >= 0)
                                throw new InvalidDataException("Unsafe package entry: " + entry.FullName);
                        }
                        var path = Path.GetFullPath(Path.Combine(root, relative));
                        if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                            throw new InvalidDataException("Package entry escapes output directory.");
                        Directory.CreateDirectory(Path.GetDirectoryName(path));
                        var source = entry.GetView();
                        using (var output = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None)) {
                            output.SetLength(entry.size);
                            long offset = 0;
                            while (offset < entry.size) {
                                var count = (int)Math.Min(buffer.Length, entry.size - offset);
                                source.Read(offset, buffer, 0, count);
                                output.Write(buffer, 0, count);
                                offset += count;
                            }
                        }
                        return buffer;
                    }, buffer => {});
                }
            }
            return 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error.ToString());
            return 1;
        }
    }
}
