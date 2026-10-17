# Windows PowerShell 5.1. Shared package import for the experimental PCVR launchers.
# Packages are identified by their own metadata, never their filename or size.
function Read-PcvrSfo([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 20 -or [BitConverter]::ToUInt32($b,0) -ne 0x46535000) { throw "Invalid param.sfo: $Path" }
    $keys=[BitConverter]::ToUInt32($b,8); $data=[BitConverter]::ToUInt32($b,12)
    $count=[BitConverter]::ToUInt32($b,16); $values=@{}
    if ($count -gt 4096 -or 20+16*$count -gt $b.Length) { throw 'Invalid SFO table.' }
    for ($i=0; $i -lt $count; $i++) {
        $at=20+16*$i; $k=$keys+[BitConverter]::ToUInt16($b,$at)
        $n=[BitConverter]::ToUInt32($b,$at+4); $v=$data+[BitConverter]::ToUInt32($b,$at+12)
        if ($k -ge $b.Length -or $v+$n -gt $b.Length) { throw 'SFO index is out of bounds.' }
        $end=[Array]::IndexOf($b,[byte]0,[int]$k)
        if ($end -lt $k) { throw 'Invalid SFO key.' }
        if ([BitConverter]::ToUInt16($b,$at+2) -eq 0x0204 -and $n -gt 0) {
            $key=[Text.Encoding]::ASCII.GetString($b,$k,$end-$k)
            $values[$key]=[Text.Encoding]::UTF8.GetString($b,$v,$n).TrimEnd([char]0)
        }
    }
    return $values
}

function Assert-PcvrImportPath([string]$GamesRoot,[string]$Path) {
    $base=[IO.Path]::GetFullPath($GamesRoot).TrimEnd('\')+'\'
    $full=[IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)) { throw "Path is outside games: $Path" }
    # Do not follow junctions/symlinks during imports, backups or promotion.
    $part=$full
    while ($part.Length -ge $base.TrimEnd('\').Length) {
        if (Test-Path -LiteralPath $part) {
            if ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Links are not allowed during import: $part" }
        }
        $part=Split-Path -Parent $part
        if (!$part) { break }
    }
    return $full
}

function Get-PcvrPackageFiles([string]$Top) {
    if (!(Test-Path -LiteralPath $Top)) { return }
    $level=@($Top)
    for ($depth=0; $depth -le 3; $depth++) {
        $next=@()
        foreach ($dir in $level) {
            Get-ChildItem -LiteralPath $dir -File -Filter '*.pkg' | Where-Object { !($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }
            foreach ($sub in (Get-ChildItem -LiteralPath $dir -Directory)) {
                if ($sub.Name.StartsWith('.') -or $sub.Name -match '\.backup-' -or
                    ($sub.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                    (Test-Path -LiteralPath (Join-Path $sub.FullName 'eboot.bin'))) { continue }
                $next += $sub.FullName
            }
        }
        $level=$next
    }
}

function Add-PcvrFailureHash([string]$Path,$Trace) {
    # Hash the complete package only on failure. Never replace the original error
    # with a diagnostic read error (missing, locked or disconnected source).
    Write-Host 'Package preparation failed. Calculating the full PKG SHA256 for diagnostics; large files may take a few minutes.'
    $stream=$null; $sha=$null
    try {
        $stream=[IO.File]::OpenRead($Path)
        $Trace.HashFileBytes=$stream.Length
        $sha=[Security.Cryptography.SHA256]::Create()
        $Trace.PackageSHA256=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant()
        Write-Host "PKG SHA256: $($Trace.PackageSHA256)"
    } catch {
        $Trace.PackageSHA256Error=$_.Exception.Message
        Write-Warning "Could not calculate the PKG SHA256: $($_.Exception.Message)"
    } finally {
        if ($sha) { $sha.Dispose() }
        if ($stream) { $stream.Dispose() }
    }
}

function Get-PcvrPackageMetadata([string]$Path,[string]$Serial,[string]$Tool,[string]$Work,[switch]$Required) {
    $trace=[ordered]@{Schema=1; UTC=[DateTime]::UtcNow.ToString('o'); Path=$Path;
        ExpectedTitle=$Serial; PowerShell=$PSVersionTable.PSVersion.ToString();
        ImporterSHA256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash;
        Tool=$Tool; Result='reading'}
    $log=Join-Path $Work 'package-metadata.jsonl'
    try {
    $head=New-Object byte[] 128
    $stream=[IO.File]::OpenRead($Path)
    try {
        $trace.FileBytes=$stream.Length; $n=0
        while ($n -lt $head.Length) {
            $read=$stream.Read($head,$n,$head.Length-$n)
            if ($read -eq 0) { break }
            $n+=$read
        }
    } finally { $stream.Dispose() }
    $trace.HeaderBytes=$n; $trace.Magic=[BitConverter]::ToString($head,0,4)
    if ($n -ne 128 -or [BitConverter]::ToUInt32($head,0) -ne 0x544E437F) {
        $trace.Result='invalid-header'
        if ($Required) { throw "This file does not have a readable PS4 PKG header (read $n bytes; signature $($trace.Magic); expected 7F-43-4E-54). Check that the copy is complete and that this is the PKG itself, not a compressed archive." }
        return
    }
    $id=[Text.Encoding]::ASCII.GetString($head,0x40,36).TrimEnd([char]0)
    $trace.ContentID=$id
    if ($id -notmatch '^[A-Z]{2}[0-9]{4}-(CUSA[0-9]{5})_') {
        $trace.Result='unsupported-content-id'
        if ($Required) { throw "The PKG content ID is not supported by this importer: '$id'. Keep the file and send the diagnostic log; its filename is not used to identify the game." }
        return
    }
    $packageSerial=$Matches[1]
    $trace.HeaderTitle=$packageSerial
    if ($Serial -and $packageSerial -ne $Serial) {
        $trace.Result='other-title'
        if ($Required) { throw "Selected $packageSerial, but this launcher requires $Serial." }
        return
    }
    $trace.ToolSHA256=(Get-FileHash -LiteralPath $Tool -Algorithm SHA256).Hash
    $lines=@(& $Tool pkg_listentries $Path 2>&1)
    $trace.ListExitCode=$LASTEXITCODE; $trace.ListOutput=@($lines | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { throw "Could not read PKG: $Path" }
    $entries=@()
    foreach ($line in $lines) {
        if ("$line" -match '^0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\d+)\s+(?:\d+\s+)?([A-Z0-9_]+)\s*$') {
            $entries += [pscustomobject]@{Index=$Matches[1]; Name=$Matches[2]}
        }
    }
    $sfo=@($entries | Where-Object Name -eq 'PARAM_SFO')
    if ($sfo.Count -ne 1) { throw "PARAM_SFO missing from PKG: $Path" }
    $temp=Join-Path $Work (([guid]::NewGuid().ToString('N'))+'.sfo')
    & $Tool pkg_extractentry --passcode ('0'*32) $Path $sfo[0].Index $temp 2>&1 | Out-Null
    $trace.MetadataExitCode=$LASTEXITCODE
    if ($LASTEXITCODE -ne 0) { throw "Could not read PKG metadata: $Path" }
    try { $info=Read-PcvrSfo $temp } finally { Remove-Item -LiteralPath $temp }
    $trace.SFO=$info
    if ($info.TITLE_ID -ne $packageSerial) { throw "Inconsistent title ID in PKG: $Path" }
    if ($info.CATEGORY -notin @('gd','gp')) {
        $trace.Result='unsupported-category'
        if ($Required) { throw "PKG $packageSerial has category '$($info.CATEGORY)'; this importer needs a base game (gd) or update (gp). Add-ons cannot replace the base game. Send the diagnostic log if this is unexpected." }
        return
    }
    $version=$null
    if (![version]::TryParse($info.APP_VER,[ref]$version)) { throw "Invalid PKG version: $Path" }
    $trace.Result='recognized'
    return [pscustomobject]@{Path=$Path; Info=$info; Version=$version; Category=$info.CATEGORY; Entries=$entries}
    } catch {
        $trace.Error=$_.Exception.Message
        if ($trace.Result -eq 'reading') { $trace.Result='error' }
        throw "$($_.Exception.Message)`nFile: $Path`nDiagnostic log: $log"
    } finally {
        if ($trace.Result -in @('invalid-header','unsupported-content-id','unsupported-category','error') -or $trace.Contains('Error')) {
            Add-PcvrFailureHash $Path $trace
        }
        try { $trace | ConvertTo-Json -Depth 6 -Compress | Add-Content -LiteralPath $log -Encoding UTF8 }
        catch { Write-Warning "Could not save package diagnostics: $($_.Exception.Message)" }
    }
}

function Select-PcvrPackage($Packages,[string]$Category) {
    $matches=@($Packages | Where-Object Category -eq $Category | Sort-Object Version -Descending)
    if (!$matches.Count) { return $null }
    $best=@($matches | Where-Object Version -eq $matches[0].Version)
    if ($best.Count -gt 1) { throw "Multiple PKGs match the same version. Keep only one in games:`n$($best.Path -join "`n")" }
    return $best[0]
}

# A user may select an update in an external folder containing both packages.
# Discover its matching base there and retain the update for the same import.
function Resolve-PcvrPackageSelection([string]$Path,[string]$Serial,[string]$Tool,[string]$Work,$ExistingPackages=@(),[switch]$AllowMissingBase) {
    $selected=Get-PcvrPackageMetadata $Path '' $Tool $Work -Required
    if ($selected.Info.TITLE_ID -ne $Serial) {
        throw "Selected $($selected.Info.TITLE_ID) version $($selected.Info.APP_VER), but this launcher requires $Serial. File: $Path"
    }
    $byPath=@{}
    foreach ($pkg in @($ExistingPackages)) { if ($pkg) { $byPath[[IO.Path]::GetFullPath($pkg.Path)]=$pkg } }
    $byPath[[IO.Path]::GetFullPath($selected.Path)]=$selected
    foreach ($file in (Get-PcvrPackageFiles (Split-Path -Parent $Path))) {
        if ($byPath.ContainsKey($file.FullName)) { continue }
        $pkg=Get-PcvrPackageMetadata $file.FullName $Serial $Tool $Work
        if ($pkg) { $byPath[$file.FullName]=$pkg }
    }
    $packages=@($byPath.Values)
    $base=if ($selected.Category -eq 'gd') { $selected } elseif (!$AllowMissingBase) { Select-PcvrPackage $packages 'gd' } else { $null }
    if (!$base -and !$AllowMissingBase) {
        throw "Selected an UPDATE for $Serial, version $($selected.Info.APP_VER). The BASE game is also required. Put both PKGs together in games, or select the base PKG. Selected file: $Path"
    }
    return [pscustomobject]@{Base=$base; Packages=$packages; Selected=$selected}
}

function Get-PcvrPackagePolicy([string]$Serial) {
    # These are this PCVR build's supported/tested configurations, not claims
    # about which update the original console game requires.
    $policies=@{
        CUSA05670=@('WipEout Omega Collection','01.07')
        CUSA03559=@('Thumper','01.06')
        CUSA04508=@('Farpoint','01.08')
        CUSA11098=@('Blood & Truth','01.11')
        CUSA06929=@('Statik','')
        CUSA02350=@('Until Dawn: Rush of Blood','')
        CUSA01690=@('PlayStation VR Worlds','')
    }
    $p=$policies[$Serial]
    if (!$p) { throw "Unknown PCVR title: $Serial" }
    return [pscustomobject]@{Title=$p[0]; Update=$p[1]; Serial=$Serial}
}

function Get-PcvrImportPlan($Policy,$Packages,$BaseInfo,$UpdateInfo,[switch]$Repair,[switch]$IgnoreUpdate) {
    $issues=@(); $base=$null; $update=$null; $baseAmbiguous=$false
    if (!$BaseInfo -or $Repair) {
        try { $base=Select-PcvrPackage $Packages 'gd' } catch { $baseAmbiguous=$true; $issues+='More than one base package matches. Keep only one base PKG in games.' }
    }
    $effectiveBase=if ($base) { $base.Info } elseif (!$Repair) { $BaseInfo } else { $null }
    $baseText=if ($effectiveBase) { "Base game: ready (version $($effectiveBase.APP_VER))" } else { 'Base game: missing - add the base game PKG to games.' }
    if ($baseAmbiguous) { $baseText='Base game: multiple copies found - keep one base PKG in games.' }
    if (!$effectiveBase) { $issues+='Add the base game. An update alone cannot run the game.' }
    $updateText='Update: not needed for this title.'
    if (!$IgnoreUpdate) {
        $required=$Policy.Update
        if ($required) {
            if ($effectiveBase -and [version]$effectiveBase.APP_VER -eq [version]$required -and
                (!$UpdateInfo -or [version]$UpdateInfo.APP_VER -eq [version]$required)) {
                $updateText="Update $required is included in the base package."
            } elseif ($UpdateInfo -and !$Repair -and [version]$UpdateInfo.APP_VER -eq [version]$required) {
                $updateText="Update: ready (installed version $required)"
            } else {
                $eligible=@($Packages | Where-Object { $_.Category -eq 'gp' -and $_.Version -eq [version]$required })
                $updateAmbiguous=$false
                try { $update=Select-PcvrPackage $eligible 'gp' } catch { $updateAmbiguous=$true; $issues+="More than one update $required matches. Keep one copy of that update in games." }
                if ($update) { $updateText="Update: ready (version $required)" }
                else {
                    $found=@($Packages | Where-Object Category -eq 'gp' | ForEach-Object { $_.Info.APP_VER } | Select-Object -Unique)
                    if ($UpdateInfo) { $found+= $UpdateInfo.APP_VER }
                    $updateText="Update ${required}: missing - this PCVR build uses this version."
                    if ($found.Count) { $updateText+=" Found: $($found -join ', ')." }
                    if ($updateAmbiguous) { $updateText="Update ${required}: multiple copies found - keep one update PKG in games." }
                    $issues+="Add the $required update for $($Policy.Title). The base game is kept."
                }
            }
        } else {
            $eligible=$Packages
            if ($UpdateInfo -and !$Repair) { $eligible=@($Packages | Where-Object { $_.Version -gt [version]$UpdateInfo.APP_VER }) }
            try { $update=Select-PcvrPackage $eligible 'gp' } catch {
                $issues+='More than one update matches. Keep one copy of that update in games.'
                $updateText='Update: multiple copies found - keep one update PKG in games.'
            }
            if ($update) { $updateText="Optional update: ready (version $($update.Info.APP_VER))" }
        }
    } else { $updateText='Update: skipped for this diagnostic run.' }
    return [pscustomobject]@{Ready=($issues.Count -eq 0); Base=$base; Update=$update; BaseText=$baseText; UpdateText=$updateText; Issues=$issues; Packages=@($Packages)}
}

function Find-PcvrTitlePackages([string]$GamesRoot,[string]$Serial,[string]$Tool,[string]$Work) {
    $packages=@(); $issues=@()
    foreach ($file in (Get-PcvrPackageFiles $GamesRoot)) {
        # A filename is a useful hint for reporting malformed candidates, never
        # authority for the title, base/update category or supported version.
        $namedForTitle=$file.Name -match [regex]::Escape($Serial)
        try {
            $meta=Get-PcvrPackageMetadata $file.FullName $Serial $Tool $Work -Required:$namedForTitle
            if ($meta) { $packages+=$meta }
        } catch { $issues+=$_.Exception.Message }
    }
    return [pscustomobject]@{Packages=@($packages); Issues=@($issues)}
}

function Show-PcvrPackageWizard($Policy,$Packages,$BaseInfo,$UpdateInfo,[string]$Tool,[string]$Work,[switch]$Repair,[switch]$IgnoreUpdate,[string]$GamesRoot,[string[]]$ScanIssues=@()) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    $state=[pscustomobject]@{Packages=@($Packages); Plan=$null}
    $form=New-Object Windows.Forms.Form
    $form.Text="Prepare $($Policy.Title)"
    $form.Add_Shown({ $this.WindowState='Normal'; $this.Activate() })
    $form.ClientSize=New-Object Drawing.Size(620,370)
    $form.StartPosition='CenterScreen'; $form.FormBorderStyle='FixedDialog'
    $form.MaximizeBox=$false; $form.MinimizeBox=$false
    $form.Font=New-Object Drawing.Font('Segoe UI',10)
    $heading=New-Object Windows.Forms.Label
    $heading.Font=New-Object Drawing.Font('Segoe UI',12,[Drawing.FontStyle]::Bold)
    $heading.Text="Add game packages for $($Policy.Title)."
    $heading.SetBounds(20,18,580,48); $form.Controls.Add($heading)
    $status=New-Object Windows.Forms.Label
    $status.SetBounds(20,74,580,110); $form.Controls.Add($status)
    $hint=New-Object Windows.Forms.Label
    $hint.Text="Put the $($Policy.Serial) PKGs in the games folder, then click Check again. Base and update files are detected automatically. Your original PKGs and saves are kept."
    $hint.SetBounds(20,192,580,60); $form.Controls.Add($hint)
    $warning=New-Object Windows.Forms.TextBox
    $warning.Multiline=$true; $warning.ReadOnly=$true; $warning.BorderStyle='None'
    $warning.ScrollBars='Vertical'; $warning.BackColor=$form.BackColor; $warning.TabStop=$false
    $warning.Visible=$false
    $warning.Add_TextChanged({ $warning.Visible=![string]::IsNullOrWhiteSpace($warning.Text) })
    $warning.ForeColor=[Drawing.Color]::DarkRed
    $warning.SetBounds(20,253,580,62); $form.Controls.Add($warning)
    $browse=New-Object Windows.Forms.Button
    $browse.Text='Open games folder'; $browse.SetBounds(20,326,150,30); $form.Controls.Add($browse)
    $rescan=New-Object Windows.Forms.Button
    $rescan.Text='Check again'; $rescan.SetBounds(180,326,130,30); $form.Controls.Add($rescan)
    $proceed=New-Object Windows.Forms.Button
    $proceed.Text='Prepare and play'; $proceed.SetBounds(324,326,160,30); $form.Controls.Add($proceed)
    $cancel=New-Object Windows.Forms.Button
    $cancel.Text='Cancel'; $cancel.SetBounds(498,326,102,30); $cancel.DialogResult='Cancel'; $form.Controls.Add($cancel)
    $form.CancelButton=$cancel; $form.AcceptButton=$proceed
    $refresh={
        $state.Plan=Get-PcvrImportPlan $Policy $state.Packages $BaseInfo $UpdateInfo -Repair:$Repair -IgnoreUpdate:$IgnoreUpdate
        $status.Text=$state.Plan.BaseText+"`r`n`r`n"+$state.Plan.UpdateText
        $proceed.Enabled=$state.Plan.Ready
    }
    & $refresh
    $warning.Text=$ScanIssues -join "`r`n`r`n"
    $browse.Add_Click({
        try { Invoke-Item -LiteralPath $GamesRoot } catch { $warning.Text=$_.Exception.Message }
    })
    $rescan.Add_Click({
        try {
            $form.UseWaitCursor=$true; $warning.Text=''
            $scan=Find-PcvrTitlePackages $GamesRoot $Policy.Serial $Tool $Work
            $state.Packages=@($scan.Packages)
            $warning.Text=$scan.Issues -join "`r`n`r`n"
            & $refresh
        } catch { $warning.Text=$_.Exception.Message }
        finally { $form.UseWaitCursor=$false }
    })
    $proceed.Add_Click({ if ($state.Plan.Ready) { $form.Tag=$state.Plan; $form.DialogResult='OK'; $form.Close() } })
    try {
        if ($form.ShowDialog() -eq 'OK') { return $form.Tag }
        return $null
    } finally { $form.Dispose() }
}

function Initialize-PcvrExtractor([string]$Root,[string]$Tool) {
    if (!(Test-Path -LiteralPath $Tool)) { return }
    $template=Join-Path $Root 'tools\pkgtool-runtime.config'
    if (Test-Path -LiteralPath $template) {
        $wanted=[IO.File]::ReadAllText($template)
        foreach ($exe in @($Tool,(Join-Path (Split-Path -Parent $Tool) 'PcvrPkgExtract.exe'))) {
            $config=$exe+'.config'
            if ((Test-Path -LiteralPath $exe) -and (!(Test-Path -LiteralPath $config) -or [IO.File]::ReadAllText($config) -ne $wanted)) {
                [IO.File]::WriteAllText($config,$wanted)
            }
        }
    }
}

function Enter-PcvrExtractorErrorMode {
    if (!('Pcvr.PackageProcess' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
namespace Pcvr {
    public static class PackageProcess {
        [DllImport("kernel32.dll")]
        public static extern uint SetErrorMode(uint mode);
    }
}
'@
    }
    # Children inherit this mode. Report their stderr/exit code in the launcher
    # instead of leaving an unrelated Windows crash dialog on the desktop.
    $previous=[Pcvr.PackageProcess]::SetErrorMode(3)
    [void][Pcvr.PackageProcess]::SetErrorMode($previous -bor 3)
    return $previous
}

function Test-PcvrStatikPak([string]$GameRoot) {
    $path=Join-Path $GameRoot 'deadlock\content\paks\deadlock-ps4.pak'
    if (!(Test-Path -LiteralPath $path)) { throw "Statik archive is missing: $path" }
    $s=[IO.File]::OpenRead($path)
    $r=New-Object IO.BinaryReader($s)
    try {
        if ($s.Length -lt 44) { throw 'Statik archive is truncated.' }
        [void]$s.Seek(-44,[IO.SeekOrigin]::End)
        if ($r.ReadUInt32() -ne 0x5A6F12E1 -or $r.ReadUInt32() -ne 3) { throw 'Statik PAK footer is unrecognized or damaged.' }
        $offset=$r.ReadInt64(); $size=$r.ReadInt64(); $hash=$r.ReadBytes(20)
        if ($offset -lt 0 -or $size -le 0 -or $size -gt 67108864 -or $offset -gt $s.Length-44 -or $size -gt $s.Length-44-$offset) {
            throw 'Corrupted index offset in pak file: Statik archive is incomplete or damaged.'
        }
        [void]$s.Seek($offset,[IO.SeekOrigin]::Begin)
        $bytes=$r.ReadBytes([int]$size); $sha=[Security.Cryptography.SHA1]::Create()
        try { $actual=$sha.ComputeHash($bytes) } finally { $sha.Dispose() }
        if ($bytes.Length -ne $size -or [BitConverter]::ToString($actual) -ne [BitConverter]::ToString($hash)) { throw 'Statik PAK index is damaged (SHA1 mismatch).' }
    } finally { $r.Dispose(); $s.Dispose() }
}

function Test-PcvrGameFolder([string]$Folder,[string]$Serial,[string]$Category) {
    if (!(Test-Path -LiteralPath (Join-Path $Folder 'eboot.bin'))) { throw "Game executable missing: $Folder\eboot.bin" }
    $info=Read-PcvrSfo (Join-Path $Folder 'sce_sys\param.sfo')
    if ($info.TITLE_ID -ne $Serial -or $info.CATEGORY -ne $Category) { throw "Wrong game or package type in the installed folder: $Folder" }
    if ($Serial -eq 'CUSA06929' -and $Category -eq 'gd') { Test-PcvrStatikPak $Folder }
    return $info
}

function Get-PcvrExtendedPath([string]$Path) {
    if ($Path.StartsWith('\\?\')) { return $Path }
    $full=[IO.Path]::GetFullPath($Path)
    if ($full.StartsWith('\\')) { return '\\?\UNC\'+$full.Substring(2) }
    return '\\?\'+$full
}

function Assert-PcvrExtractedTree([string]$Folder) {
    # PowerShell 5.1's filesystem provider may miss or reject long names.
    # Enumerate one directory at a time with extended Win32 paths, checking
    # every entry before descending so that junctions are never followed.
    $pending=New-Object 'Collections.Generic.Stack[string]'
    $pending.Push((Get-PcvrExtendedPath $Folder))
    while ($pending.Count) {
        $dir=$pending.Pop()
        if ([IO.File]::GetAttributes($dir) -band [IO.FileAttributes]::ReparsePoint) { throw 'Unexpected link in extracted game.' }
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($dir)) {
            $attributes=[IO.File]::GetAttributes($entry)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unexpected link in extracted game.' }
            if ($attributes -band [IO.FileAttributes]::Directory) { $pending.Push($entry) }
        }
    }
}

function Install-PcvrPackage($Package,[string]$Target,[string]$GamesRoot,[string]$Tool,[string]$Work) {
    try {
    $Target=Assert-PcvrImportPath $GamesRoot $Target
    # Rush has filenames close to MAX_PATH even at a short install location.
    # Keep staging shorter than games/<Title>-CUSAxxxxx/uroot, not nested
    # beneath the metadata work directory. The import lock still serializes it.
    $stage=Assert-PcvrImportPath $GamesRoot (Join-Path $GamesRoot ('.pkg-'+[guid]::NewGuid().ToString('N').Substring(0,8)))
    if (Test-Path -LiteralPath $stage) { throw 'Temporary folder already exists. Please try again.' }
    [void][IO.Directory]::CreateDirectory($stage)
    $out=Join-Path $stage 'f'
    $drive=New-Object IO.DriveInfo([IO.Path]::GetPathRoot($Target))
    # Conservative estimate; package compression varies. No title-specific Astro ratio.
    $needed=(Get-Item -LiteralPath $Package.Path).Length*3 + 1GB
    if ($drive.AvailableFreeSpace -lt $needed) { throw ('Not enough free space. Free at least {0:N1} GB on {1} and try again. Your PKG is unchanged.' -f ($needed/1GB),$drive.Name) }
    Write-Host "Extracting $($Package.Info.TITLE) $($Package.Info.APP_VER). Please wait; your original PKG is kept."
    # Blood & Truth includes filenames exceeding MAX_PATH even in a short
    # installation folder. Both the runtime switches and prefix are needed.
    $extractor=Join-Path (Split-Path -Parent $Tool) 'PcvrPkgExtract.exe'
    if (!(Test-Path -LiteralPath $extractor)) { throw 'The read-only PKG extractor is missing. Extract the complete current PCVR build again.' }
    $argsLine='"'+$Package.Path+'" "'+(Get-PcvrExtendedPath $out)+'" '+('0'*32)
    # ProcessStartInfo treats brackets literally (Start-Process redirection in PS5 does not).
    $si=New-Object Diagnostics.ProcessStartInfo
    $si.FileName=$extractor; $si.Arguments=$argsLine; $si.UseShellExecute=$false
    $si.CreateNoWindow=$true; $si.RedirectStandardOutput=$true; $si.RedirectStandardError=$true
    $p=New-Object Diagnostics.Process
    $p.StartInfo=$si; [void]$p.Start()
    $stdout=$p.StandardOutput.ReadToEndAsync(); $stderr=$p.StandardError.ReadToEndAsync()
    try {
        while (!$p.WaitForExit(500)) { Write-Progress -Activity "Extracting $($Package.Info.TITLE)" -Status 'Closing this window interrupts preparation. Start the BAT again to retry.' }
        $p.WaitForExit()
        [IO.File]::WriteAllText((Join-Path $stage 'output.txt'),$stdout.Result)
        [IO.File]::WriteAllText((Join-Path $stage 'errors.txt'),$stderr.Result)
        if ($p.ExitCode -ne 0) {
            if ($stderr.Result -match 'UnauthorizedAccessException') {
                throw "Windows denied access while reading the PKG or writing the extracted files. The PKG is opened read-only. Check access to the source file and write permission in games; a writable local folder can help isolate a shared-drive permission problem. Your PKG is unchanged. Details: $stage\errors.txt"
            }
            if ($stderr.Result -match 'PathTooLongException') {
                throw "Extraction stopped because a file path is too long. Move the complete PCVR folder to a short location such as C:\VR\AstroQuest and try again. Your PKGs are unchanged. Details: $stage\errors.txt"
            }
            throw "Extraction did not complete (code $($p.ExitCode)). Your PKGs are unchanged and the incomplete copy will not be launched. Details: $stage\errors.txt"
        }
    } finally {
        if (!$p.HasExited) { $p.Kill(); $p.WaitForExit() }
        $p.Dispose(); Write-Progress -Activity 'Extraction' -Completed
    }
    $unpacked=Join-Path $out 'uroot'
    if (!(Test-Path -LiteralPath $unpacked)) { $unpacked=$out }
    $skip=@('DIGESTS','ENTRY_KEYS','IMAGE_KEY','GENERAL_DIGESTS','METAS','ENTRY_NAMES','LICENSE_DAT','LICENSE_INFO')
    foreach ($entry in $Package.Entries) {
        if ($entry.Name -in $skip -or $entry.Name.EndsWith('_DDS')) { continue }
        $name=$entry.Name.ToLowerInvariant().Replace('__','\'); $at=$name.LastIndexOf('_')
        if ($at -gt 0) { $name=$name.Substring(0,$at)+'.'+$name.Substring($at+1) }
        if ($name.StartsWith('playgo_')) { $name='playgo-'+$name.Substring(7) }
        $dest=Assert-PcvrImportPath $GamesRoot (Join-Path (Join-Path $unpacked 'sce_sys') $name)
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $dest))
        & $Tool pkg_extractentry --passcode ('0'*32) $Package.Path $entry.Index $dest 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Game information could not be extracted ($($entry.Name)). Your PKG is unchanged. Details: $stage" }
    }
    $info=Test-PcvrGameFolder $unpacked $Package.Info.TITLE_ID $Package.Category
    if ($info.APP_VER -ne $Package.Info.APP_VER) { throw 'The extracted game version does not match the PKG. This copy will not be started.' }
    Assert-PcvrExtractedTree $unpacked
    # Promote only a completed and validated extraction. Preserve the previous folder.
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $Target))
    $backup=$null
    if (Test-Path -LiteralPath $Target) {
        $backup=Assert-PcvrImportPath $GamesRoot ($Target+'.backup-'+[guid]::NewGuid().ToString('N'))
        Move-Item -LiteralPath $Target -Destination $backup
    }
    try { Move-Item -LiteralPath (Assert-PcvrImportPath $GamesRoot $unpacked) -Destination $Target }
    catch {
        if ($backup -and !(Test-Path -LiteralPath $Target)) { Move-Item -LiteralPath $backup -Destination $Target }
        throw
    }
    if ($backup) { Write-Host "Previous copy kept at $backup" }
    # Only this successful job's private staging directory is removed; failures keep diagnostics.
    Remove-Item -LiteralPath (Assert-PcvrImportPath $GamesRoot $stage) -Recurse -Force
    Write-Host "Ready: $Target"
    } catch {
        $failure=$_
        $trace=[ordered]@{Schema=1; UTC=[DateTime]::UtcNow.ToString('o');
            Path=$Package.Path; Result='extraction-failed'; Target=$Target;
            Error=$failure.Exception.Message}
        Add-PcvrFailureHash $Package.Path $trace
        $log=Join-Path $Work 'package-metadata.jsonl'
        try { $trace | ConvertTo-Json -Depth 6 -Compress | Add-Content -LiteralPath $log -Encoding UTF8 }
        catch { Write-Warning "Could not save package diagnostics: $($_.Exception.Message)" }
        throw "$($failure.Exception.Message)`nDiagnostic log: $log"
    }
}

function Initialize-PcvrGame([string]$Root,[string]$Game,[switch]$IgnoreUpdate,[switch]$Repair,[switch]$NonInteractive) {
    $gamesRoot=Join-Path $Root 'games'; $folder=Split-Path -Parent $Game
    if ($folder -notmatch '(CUSA[0-9]{5})[\\/]uroot$') { throw 'Unrecognized game path.' }
    $serial=$Matches[1]; $tool=Join-Path $Root 'pc-vr\pkgtool\PkgTool.exe'
    Initialize-PcvrExtractor $Root $tool
    [void][IO.Directory]::CreateDirectory($gamesRoot)
    $null=Assert-PcvrImportPath $gamesRoot $folder
    # Prevent two launchers from writing game folders concurrently. No stale-lock deletion needed.
    $lockPath=Assert-PcvrImportPath $gamesRoot (Join-Path $gamesRoot '.pcvr-import.lock')
    try { $lock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None) }
    catch { throw 'Another launcher is preparing a game. Wait for it to finish, then try again.' }
    $work=Assert-PcvrImportPath $gamesRoot (Join-Path $gamesRoot ('.pkg-import-'+[guid]::NewGuid().ToString('N').Substring(0,8)))
    $previousErrorMode=$null
    try {
        $previousErrorMode=Enter-PcvrExtractorErrorMode
        if (Test-Path -LiteralPath $work) { throw 'Temporary folder already exists. Please try again.' }
        [void][IO.Directory]::CreateDirectory($work)
        $baseInfo=$null; $baseError=$null
        try { $baseInfo=Test-PcvrGameFolder $folder $serial 'gd' } catch { $baseError=$_.Exception.Message }
        $scan=Find-PcvrTitlePackages $gamesRoot $serial $tool $work
        $packages=@($scan.Packages)
        foreach ($issue in $scan.Issues) { Write-Warning $issue }
        $policy=Get-PcvrPackagePolicy $serial
        $updateFolder=$folder+'-UPDATE'; $updateInfo=$null
        if (!$IgnoreUpdate -and (Test-Path -LiteralPath $updateFolder)) {
            try { $updateInfo=Test-PcvrGameFolder $updateFolder $serial 'gp' } catch {
                if (!$policy.Update -and !@($packages | Where-Object Category -eq 'gp').Count) {
                    throw "The installed update is incomplete. Add its update PKG to games and start again to repair it. Folder: $updateFolder"
                }
            }
        }
        $plan=Get-PcvrImportPlan $policy $packages $baseInfo $updateInfo -Repair:$Repair -IgnoreUpdate:$IgnoreUpdate
        if (!$plan.Ready) {
            if ($NonInteractive) { throw ($plan.BaseText+"`n"+$plan.UpdateText+"`n"+(($plan.Issues+$scan.Issues) -join "`n")) }
            if (!(Test-Path -LiteralPath $tool)) { throw 'PkgTool is missing. Extract the complete PCVR ZIP again.' }
            $plan=Show-PcvrPackageWizard $policy $packages $baseInfo $updateInfo $tool $work -Repair:$Repair -IgnoreUpdate:$IgnoreUpdate -GamesRoot $gamesRoot -ScanIssues $scan.Issues
            if ($null -eq $plan) { Write-Host 'Preparation cancelled. No game was started.'; return $false }
        }
        if ($plan.Base) {
            if ($baseError) { Write-Host "Preparing or repairing game files: $baseError" }
            Install-PcvrPackage $plan.Base $folder $gamesRoot $tool $work
        }
        if ($plan.Update) {
            Install-PcvrPackage $plan.Update $updateFolder $gamesRoot $tool $work
        }
        return $true
    } finally {
        if ($null -ne $previousErrorMode) { [void][Pcvr.PackageProcess]::SetErrorMode($previousErrorMode) }
        $lock.Dispose()
        # Keep failed extraction logs and partial files; never mistake them for installed games.
        if ((Test-Path -LiteralPath $work) -and !(Get-ChildItem -LiteralPath $work -Force)) { Remove-Item -LiteralPath (Assert-PcvrImportPath $gamesRoot $work) }
    }
}
