# Makes small documents with the writers Windows has, for
# tests/support/document_samples.cpp (embed_document_samples.py):
#   edge_print.pdf          Microsoft Edge (Skia/PDF) printing an HTML page with a JPEG image
#   mspdf_shapes.pdf        Microsoft Print to PDF (the printer every Windows has): shapes and the JPEG
#   xps_shapes.oxps         Microsoft XPS Document Writer: the same page as OpenXPS (a ZIP)
#   stg_word.doc            Windows' structured storage (StgCreateStorageEx): Word's stream names, 512-byte sectors
#   stg_excel_v4.xls        the same with Excel's stream names and 4096-byte sectors (version 4)
#   stg_shrunk.doc          a file whose streams shrank after it was saved: free sectors inside it
#   wi_created.msi          Windows Installer (WindowsInstaller.Installer): a new database with one table
# make_document_samples.sh then writes qpdf's variants of mspdf_shapes.pdf.
#
# Usage: powershell -File make_document_samples.ps1 <output folder>
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
Add-Type -AssemblyName System.Drawing

# The JPEG the pages show: a 48x32 gradient from GDI+'s encoder.
$bitmap = New-Object System.Drawing.Bitmap 48, 32
for ($x = 0; $x -lt 48; $x++) {
    for ($y = 0; $y -lt 32; $y++) {
        $bitmap.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(($x * 5) % 256, ($y * 8) % 256, (($x + $y) * 3) % 256))
    }
}
$photo = Join-Path $Out 'photo.jpg'
$bitmap.Save($photo, [System.Drawing.Imaging.ImageFormat]::Jpeg)
$bitmap.Dispose()

# Edge, headless.
$page = Join-Path $Out 'page.html'
Set-Content -Path $page -Encoding ascii -Value ('<!doctype html><html><head><meta charset="utf-8"><title>Recovered</title>' +
    '<style>body{font-family:Arial;margin:40px}</style></head><body><p>Recovered</p>' +
    '<img src="photo.jpg" width="96" height="64"></body></html>')
$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
$profile = Join-Path $Out 'edge-profile'
Start-Process -FilePath $edge -Wait -ArgumentList @('--headless', '--disable-gpu', '--no-pdf-header-footer',
    "--user-data-dir=$profile", "--print-to-pdf=$(Join-Path $Out 'edge_print.pdf')", ([System.Uri]$page).AbsoluteUri)
Remove-Item -Recurse -Force $profile

# The printers, through System.Drawing.Printing: shapes and the image, no
# text (a font would be embedded whole).
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Printing;
public static class SamplePrinter {
    public static void Print(string printer, string file, string imagePath) {
        var doc = new PrintDocument();
        doc.PrinterSettings.PrinterName = printer;
        doc.PrinterSettings.PrintToFile = true;
        doc.PrinterSettings.PrintFileName = file;
        doc.PrintController = new StandardPrintController();
        doc.PrintPage += (s, e) => {
            e.Graphics.FillRectangle(Brushes.SteelBlue, 50, 50, 200, 30);
            e.Graphics.DrawEllipse(Pens.DarkRed, 60, 90, 120, 60);
            using (var image = Image.FromFile(imagePath)) {
                e.Graphics.DrawImage(image, 50, 170, 48, 32);
            }
            e.HasMorePages = false;
        };
        doc.Print();
    }
}
'@
[SamplePrinter]::Print('Microsoft Print to PDF', (Join-Path $Out 'mspdf_shapes.pdf'), $photo)
[SamplePrinter]::Print('Microsoft XPS Document Writer', (Join-Path $Out 'xps_shapes.oxps'), $photo)

# Windows' structured storage (the compound file implementation of OLE).
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;

[ComImport, Guid("0000000b-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IStorageSample {
    void CreateStream([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint r1, uint r2, out IStream stream);
    void OpenStream([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr r1, uint mode, uint r2, out IStream stream);
    void CreateStorage([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint r1, uint r2, out IStorageSample storage);
    void OpenStorage([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr priority, uint mode, IntPtr exclude, uint r, out IStorageSample storage);
    void CopyTo(uint count, IntPtr exclude, IntPtr snb, IStorageSample dest);
    void MoveElementTo([MarshalAs(UnmanagedType.LPWStr)] string name, IStorageSample dest, [MarshalAs(UnmanagedType.LPWStr)] string newName, uint flags);
    void Commit(uint flags);
    void Revert();
    void EnumElements(uint r1, IntPtr r2, uint r3, out IntPtr e);
    void DestroyElement([MarshalAs(UnmanagedType.LPWStr)] string name);
    void RenameElement([MarshalAs(UnmanagedType.LPWStr)] string oldName, [MarshalAs(UnmanagedType.LPWStr)] string newName);
    void SetElementTimes([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr c, IntPtr a, IntPtr m);
    void SetClass(ref Guid clsid);
    void SetStateBits(uint bits, uint mask);
    void Stat(out System.Runtime.InteropServices.ComTypes.STATSTG stat, uint flag);
}

[StructLayout(LayoutKind.Sequential)]
public struct SampleStgOptions {
    public ushort Version;
    public ushort Reserved;
    public uint SectorSize;
    public IntPtr TemplateFile;
}

public static class SampleCompoundFiles {
    const uint ReadWrite = 0x2, ShareExclusive = 0x10, CreateNew = 0x1000, DocFile = 5;
    static Guid storageIid = new Guid("0000000b-0000-0000-C000-000000000046");

    [DllImport("ole32.dll")]
    static extern int StgCreateStorageEx([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint format, uint attrs,
        ref SampleStgOptions options, IntPtr security, ref Guid iid, out IStorageSample storage);
    [DllImport("ole32.dll")]
    static extern int StgOpenStorageEx([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint format, uint attrs,
        IntPtr options, IntPtr security, ref Guid iid, out IStorageSample storage);

    static byte[] Pattern(int size, int seed) {
        var data = new byte[size];
        uint x = (uint)seed * 2654435761u + 1;
        for (int i = 0; i < size; ++i) { x = x * 1103515245u + 12345u; data[i] = (byte)(x >> 16); }
        return data;
    }

    // A new compound file of these streams (pattern data of these sizes).
    public static void Make(string path, uint sectorSize, string[] names, int[] sizes) {
        var options = new SampleStgOptions { Version = 1, SectorSize = sectorSize };
        IStorageSample storage;
        Marshal.ThrowExceptionForHR(StgCreateStorageEx(path, ReadWrite | ShareExclusive | CreateNew, DocFile, 0,
            ref options, IntPtr.Zero, ref storageIid, out storage));
        for (int i = 0; i < names.Length; ++i) {
            IStream stream;
            storage.CreateStream(names[i], ReadWrite | ShareExclusive | CreateNew, 0, 0, out stream);
            byte[] data = Pattern(sizes[i], i + 1);
            stream.Write(data, data.Length, IntPtr.Zero);
            Marshal.ReleaseComObject(stream);
        }
        storage.Commit(0);
        Marshal.ReleaseComObject(storage);
    }

    // Opens the file again and sets these streams' sizes.
    public static void Resize(string path, string[] names, long[] sizes) {
        IStorageSample storage;
        Marshal.ThrowExceptionForHR(StgOpenStorageEx(path, ReadWrite | ShareExclusive, DocFile, 0, IntPtr.Zero,
            IntPtr.Zero, ref storageIid, out storage));
        for (int i = 0; i < names.Length; ++i) {
            IStream stream;
            storage.OpenStream(names[i], IntPtr.Zero, ReadWrite | ShareExclusive, 0, out stream);
            stream.SetSize(sizes[i]);
            Marshal.ReleaseComObject(stream);
        }
        storage.Commit(0);
        Marshal.ReleaseComObject(storage);
    }
}
'@
$five = [string][char]5
$one = [string][char]1
[SampleCompoundFiles]::Make((Join-Path $Out 'stg_word.doc'), 512,
    @('WordDocument', '1Table', ($five + 'SummaryInformation'), ($one + 'CompObj')), @(5000, 2000, 300, 100))
[SampleCompoundFiles]::Make((Join-Path $Out 'stg_excel_v4.xls'), 4096, @('Workbook', ($five + 'SummaryInformation')),
    @(5000, 200))
[SampleCompoundFiles]::Make((Join-Path $Out 'stg_shrunk.doc'), 512, @('WordDocument', '1Table'), @(9000, 5000))
[SampleCompoundFiles]::Resize((Join-Path $Out 'stg_shrunk.doc'), @('WordDocument', '1Table'), [long[]]@(4100, 4500))

# Windows Installer: a new database with a Property table and summary information.
$msi = [string](Join-Path $Out 'wi_created.msi')
$installer = New-Object -ComObject WindowsInstaller.Installer
$database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($msi, 3))
foreach ($sql in @(
        'CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)',
        "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductName', 'Recovered')")) {
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @($sql))
    $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
    $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
}
$summary = $database.GetType().InvokeMember('SummaryInformation', 'GetProperty', $null, $database, @(4))
$summary.GetType().InvokeMember('Property', 'SetProperty', $null, $summary, @(2, 'Recovered')) | Out-Null
$summary.GetType().InvokeMember('Persist', 'InvokeMethod', $null, $summary, $null) | Out-Null
$database.GetType().InvokeMember('Commit', 'InvokeMethod', $null, $database, $null) | Out-Null
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($summary) | Out-Null
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($database) | Out-Null
[System.Runtime.InteropServices.Marshal]::ReleaseComObject($installer) | Out-Null
[System.GC]::Collect()
[System.GC]::WaitForPendingFinalizers()

Remove-Item $page, $photo
Get-ChildItem $Out | Select-Object Name, Length | Format-Table -AutoSize
