using MagicStudio.FFmpegPlus;
using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.UI;
using Microsoft.Graphics.Canvas.UI.Xaml;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System;
using Windows.Foundation;
using Windows.Graphics.DirectX;

namespace MagicStudio.UI;

public sealed partial class MediaPlayerControl : UserControl, IDisposable
{
    private Player?       _player;
    private CanvasBitmap? _frameBitmap;
    private bool          _playing;
    private bool          _seekPending;
    private bool          _disposed;

    // Prevents slider ValueChanged from re-triggering during programmatic updates.
    private bool _suppressSlider;

    public MediaPlayerControl()
    {
        InitializeComponent();
        Unloaded += OnUnloaded;
    }

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    public bool Open(string path)
    {
        _player?.Dispose();
        _player = new Player();
        bool ok = _player.Open(path);
        if (ok)
        {
            double dur = _player.Duration;
            _suppressSlider = true;
            _progressSlider.Maximum = dur > 0 ? dur : 1;
            _progressSlider.Value   = 0;
            _suppressSlider = false;
            UpdateTimeLabel(0, dur);
        }
        return ok;
    }

    public void Play()
    {
        if (_player is null) return;
        _player.Play();
        _playing = true;
        _playPauseBtn.Content = "\uE769"; // Pause glyph
        _canvas.Invalidate();
    }

    public void Pause()
    {
        _player?.Pause();
        _playing = false;
        _playPauseBtn.Content = "\uE768"; // Play glyph
    }

    // -------------------------------------------------------------------------
    // Win2D canvas callbacks
    // -------------------------------------------------------------------------

    private void _Canvas_CreateResources(CanvasControl sender, CanvasCreateResourcesEventArgs args)
    {
        // Nothing to pre-allocate; _frameBitmap is created lazily on first frame.
    }

    private void _Canvas_Draw(CanvasControl sender, CanvasDrawEventArgs args)
    {
        if (_player is null || !_playing)
            return;

        long pts = _player.GetAudioPositionUs();

        if (_player.TryGetFrame(pts, out byte[]? bgra, out int w, out int h) && bgra is not null)
        {
            if (_frameBitmap is null
                || (int)_frameBitmap.SizeInPixels.Width  != w
                || (int)_frameBitmap.SizeInPixels.Height != h)
            {
                _frameBitmap?.Dispose();
                _frameBitmap = CanvasBitmap.CreateFromBytes(
                    sender, bgra, w, h,
                    DirectXPixelFormat.B8G8R8A8UIntNormalized);
            }
            else
            {
                // Reuse the existing GPU texture — avoids reallocation every frame.
                _frameBitmap.SetPixelBytes(bgra);
            }

            // Update progress slider (≈ every frame, not worth throttling here).
            UpdateProgress(pts);
        }

        if (_frameBitmap is not null)
        {
            args.DrawingSession.DrawImage(
                _frameBitmap,
                new Rect(0, 0, sender.ActualWidth, sender.ActualHeight));
        }

        // Keep the render loop alive while playing.
        if (_playing)
            sender.Invalidate();
    }

    // -------------------------------------------------------------------------
    // Controls
    // -------------------------------------------------------------------------

    private void _PlayPause_Click(object sender, RoutedEventArgs e)
    {
        if (_playing) Pause();
        else          Play();
    }

    private void _Progress_ValueChanged(object sender,
        Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_suppressSlider || _player is null) return;
        // Seeking is not implemented in this initial version; extend MediaDecoder
        // with avformat_seek_file() to support it.
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    private void UpdateProgress(long pts_us)
    {
        double seconds = pts_us / 1_000_000.0;
        double duration = _player?.Duration ?? 0;

        _suppressSlider = true;
        _progressSlider.Value = Math.Clamp(seconds, 0, _progressSlider.Maximum);
        _suppressSlider = false;

        UpdateTimeLabel(seconds, duration);
    }

    private static string FormatTime(double seconds)
    {
        var ts = TimeSpan.FromSeconds(seconds);
        return ts.Hours > 0
            ? $"{ts.Hours}:{ts.Minutes:D2}:{ts.Seconds:D2}"
            : $"{ts.Minutes}:{ts.Seconds:D2}";
    }

    private void UpdateTimeLabel(double current, double total)
    {
        _timeText.Text = $"{FormatTime(current)} / {FormatTime(total)}";
    }

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    private void OnUnloaded(object sender, RoutedEventArgs e) => Dispose();

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _player?.Dispose();
        _player = null;
        _frameBitmap?.Dispose();
        _frameBitmap = null;
    }
}
