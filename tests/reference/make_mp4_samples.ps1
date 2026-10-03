# Makes small MP4 files with Windows' own H.264/AAC encoders and MP4 writer
# (Media Foundation, through the WinRT MediaComposition and MediaTranscoder, as
# the Photos app, the Camera app and other Windows apps write them), for the
# samples embedded in tests/support/mp4_samples.cpp and for the reference
# corpus of Mp4ReferenceTest. Companion of make_mp4_samples.sh (FFmpeg, GPAC).
#
# Usage: powershell -File make_mp4_samples.ps1 <output directory> [large]
#   Without "large" the clips last half a second at a small size; with
#   "large", 20 seconds at 640x360.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Size = "small"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Runtime.WindowsRuntime

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if ($Size -eq "large") {
    $seconds = 20.0; $width = 640; $height = 360
} else {
    $seconds = 0.5; $width = 96; $height = 64
}

[Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Editing.MediaComposition, Windows.Media.Editing, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Editing.MediaClip, Windows.Media.Editing, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Editing.BackgroundAudioTrack, Windows.Media.Editing, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime] | Out-Null
[Windows.UI.Colors, Windows.UI, ContentType = WindowsRuntime] | Out-Null

# PowerShell 5.1 waits for WinRT operations through AsTask.
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq "AsTask" -and $_.GetParameters().Count -eq 1
}
function Await($Operation, [Type]$Result) {
    $method = ($asTask | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq "IAsyncOperation``1" })[0]
    $task = $method.MakeGenericMethod($Result).Invoke($null, @($Operation))
    $task.Wait(-1) | Out-Null
    $task.Result
}
function AwaitWithProgress($Operation, [Type]$Result) {
    $method = ($asTask | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq "IAsyncOperationWithProgress``2" })[0]
    $task = $method.MakeGenericMethod($Result, [double]).Invoke($null, @($Operation))
    $task.Wait(-1) | Out-Null
    $task.Result
}
function AwaitAction($Operation) {
    $method = ($asTask | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq "IAsyncActionWithProgress``1" })[0]
    $task = $method.MakeGenericMethod([double]).Invoke($null, @($Operation))
    $task.Wait(-1) | Out-Null
}

# A 44.1 kHz stereo WAV of a 440 Hz tone, for the audio track.
$tone = Join-Path $env:TEMP "recovery_mp4_tone.wav"
$rate = 44100
$frames = [int]($rate * $seconds)
$stream = [System.IO.File]::Create($tone)
$writer = New-Object System.IO.BinaryWriter $stream
$writer.Write([Text.Encoding]::ASCII.GetBytes("RIFF")); $writer.Write([int](36 + $frames * 4))
$writer.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt ")); $writer.Write([int]16)
$writer.Write([int16]1); $writer.Write([int16]2); $writer.Write([int]$rate); $writer.Write([int]($rate * 4))
$writer.Write([int16]4); $writer.Write([int16]16)
$writer.Write([Text.Encoding]::ASCII.GetBytes("data")); $writer.Write([int]($frames * 4))
for ($i = 0; $i -lt $frames; $i++) {
    $sample = [int16](8000 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate))
    $writer.Write($sample); $writer.Write($sample)
}
$writer.Close()

$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($Out)) ([Windows.Storage.StorageFolder])
$replace = [Windows.Storage.CreationCollisionOption]::ReplaceExisting
# WinRT collections: PowerShell 5.1 reaches their methods through the .NET interface.
function AddTo($Collection, $Item, [Type]$ItemType) {
    $interface = [System.Collections.Generic.ICollection``1].MakeGenericType($ItemType)
    $interface.GetMethod("Add").Invoke($Collection, @($Item)) | Out-Null
}
function Profile() {
    $profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateMp4(
        [Windows.Media.MediaProperties.VideoEncodingQuality]::Qvga)
    $profile.Video.Width = $width
    $profile.Video.Height = $height
    $profile.Video.Bitrate = 64000
    $profile.Video.FrameRate.Numerator = 10
    $profile.Video.FrameRate.Denominator = 1
    $profile
}

# A coloured clip, with and without the tone behind it (MediaComposition).
foreach ($name in @("mediafoundation_h264_aac.mp4", "mediafoundation_video_only.mp4")) {
    $composition = New-Object Windows.Media.Editing.MediaComposition
    $clip = [Windows.Media.Editing.MediaClip]::CreateFromColor([Windows.UI.Colors]::SteelBlue,
                                                              [TimeSpan]::FromSeconds($seconds))
    AddTo $composition.Clips $clip ([Windows.Media.Editing.MediaClip])
    $profile = Profile
    if ($name -like "*aac*") {
        $toneFile = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($tone)) ([Windows.Storage.StorageFile])
        $track = Await ([Windows.Media.Editing.BackgroundAudioTrack]::CreateFromFileAsync($toneFile)) `
            ([Windows.Media.Editing.BackgroundAudioTrack])
        AddTo $composition.BackgroundAudioTracks $track ([Windows.Media.Editing.BackgroundAudioTrack])
    } else {
        $profile.Audio = $null
    }
    $target = Await ($folder.CreateFileAsync($name, $replace)) ([Windows.Storage.StorageFile])
    $trimming = [Windows.Media.Editing.MediaTrimmingPreference]::Precise
    $failure = AwaitWithProgress ($composition.RenderToFileAsync($target, $trimming, $profile)) `
        ([Windows.Media.Transcoding.TranscodeFailureReason])
    if ($failure -ne [Windows.Media.Transcoding.TranscodeFailureReason]::None) {
        throw "Media Foundation could not write $name ($failure)"
    }
}

# The first file transcoded once more by MediaTranscoder.
$source = Await ($folder.GetFileAsync("mediafoundation_h264_aac.mp4")) ([Windows.Storage.StorageFile])
$target = Await ($folder.CreateFileAsync("mediafoundation_transcoded.mp4", $replace)) ([Windows.Storage.StorageFile])
$transcoder = New-Object Windows.Media.Transcoding.MediaTranscoder
$prepared = Await ($transcoder.PrepareFileTranscodeAsync($source, $target, (Profile))) `
    ([Windows.Media.Transcoding.PrepareTranscodeResult])
if (-not $prepared.CanTranscode) {
    throw "Media Foundation cannot transcode ($($prepared.FailureReason))"
}
AwaitAction $prepared.TranscodeAsync()
Remove-Item $tone

Get-ChildItem $Out -Filter "mediafoundation*" | Format-Table Name, Length
