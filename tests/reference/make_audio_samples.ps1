# Makes small audio files with Windows' own encoders, for the samples embedded
# in tests/support/audio_samples.cpp and for the reference corpus of
# AudioReferenceTest. Companion of make_audio_samples.sh (LAME, FFmpeg, faac,
# fdkaac, SoX):
#  * the speech synthesizer (System.Speech) writes WAV: PCM, mu-law, A-law;
#  * Media Foundation, through the WinRT MediaTranscoder, writes M4A (AAC) and
#    MP3 from the PCM file, as the Voice Recorder and other Windows apps do.
#
# Usage: powershell -File make_audio_samples.ps1 <output directory> [large]
#   Without "large" the speech is a single word; with "large", a few sentences.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Size = "small"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech
Add-Type -AssemblyName System.Runtime.WindowsRuntime

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if ($Size -eq "large") {
    $text = "The quick brown fox jumps over the lazy dog. " * 4
} else {
    $text = "Hi."
}

# WAV from the speech synthesizer.
function Speak([string]$Path, $Format) {
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    try {
        $synth.Rate = 10
        $synth.SetOutputToWaveFile($Path, $Format)
        $synth.Speak($text)
    } finally {
        $synth.Dispose()
    }
}
$encoding = [System.Speech.AudioFormat.EncodingFormat]
$channels = [System.Speech.AudioFormat.AudioChannel]::Mono
$pcm = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 8000, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), $channels
Speak (Join-Path $Out "speech_pcm.wav") $pcm
$ulaw = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo $encoding::ULaw, 8000, 8, 1, 8000, 1, $null
Speak (Join-Path $Out "speech_ulaw.wav") $ulaw
$alaw = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo $encoding::ALaw, 8000, 8, 1, 8000, 1, $null
Speak (Join-Path $Out "speech_alaw.wav") $alaw

# M4A and MP3 from Media Foundation. PowerShell 5.1 waits for WinRT operations through AsTask.
[Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime] | Out-Null
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq "AsTask" -and $_.GetParameters().Count -eq 1
}
function Await($Operation, [Type]$Result) {
    $method = ($asTask | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq "IAsyncOperation``1" })[0]
    $task = $method.MakeGenericMethod($Result).Invoke($null, @($Operation))
    $task.Wait(-1) | Out-Null
    $task.Result
}
function AwaitProgress($Operation) {
    $method = ($asTask | Where-Object { $_.GetParameters()[0].ParameterType.Name -eq "IAsyncActionWithProgress``1" })[0]
    $task = $method.MakeGenericMethod([double]).Invoke($null, @($Operation))
    $task.Wait(-1) | Out-Null
}
# Media Foundation's AAC encoder takes 44.1 or 48 kHz input, so speak once more at 44.1 kHz.
$source44 = Join-Path $env:TEMP "recovery_speech_44k.wav"
$pcm44 = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo 44100, ([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), $channels
Speak $source44 $pcm44
$source = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($source44)) ([Windows.Storage.StorageFile])
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($Out)) ([Windows.Storage.StorageFolder])
$quality = [Windows.Media.MediaProperties.AudioEncodingQuality]::Low
$profiles = @{
    "mediafoundation_aac.m4a" = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateM4a($quality)
    "mediafoundation.mp3"     = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateMp3($quality)
}
foreach ($name in $profiles.Keys) {
    $replace = [Windows.Storage.CreationCollisionOption]::ReplaceExisting
    $target = Await ($folder.CreateFileAsync($name, $replace)) ([Windows.Storage.StorageFile])
    $transcoder = New-Object Windows.Media.Transcoding.MediaTranscoder
    $prepared = Await ($transcoder.PrepareFileTranscodeAsync($source, $target, $profiles[$name])) `
        ([Windows.Media.Transcoding.PrepareTranscodeResult])
    if (-not $prepared.CanTranscode) {
        throw "Media Foundation cannot write $name ($($prepared.FailureReason))"
    }
    AwaitProgress $prepared.TranscodeAsync()
}
Remove-Item $source44

Get-ChildItem $Out -Include speech_*, mediafoundation* -Recurse | Format-Table Name, Length
