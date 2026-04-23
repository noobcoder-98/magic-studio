using Microsoft.UI.Xaml;
using System;
using Windows.Storage.Pickers;

namespace MagicStudio.UI;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;

        // WinUI 3 does not fire Unloaded on the window's content tree when the
        // window closes, so we must explicitly tear down the player here.
        // Without this, the native decoder + refresh + SDL audio threads stay
        // live while the CLR begins unloading DLLs around them, surfacing as
        // an unhandled Win32 exception during shutdown.
        Closed += (_, _) => MediaPlayer.Dispose();
    }

    private async void OpenFile_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        WinRT.Interop.InitializeWithWindow.Initialize(picker,
            WinRT.Interop.WindowNative.GetWindowHandle(this));

        picker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
        picker.FileTypeFilter.Add(".mp4");
        picker.FileTypeFilter.Add(".mkv");
        picker.FileTypeFilter.Add(".avi");
        picker.FileTypeFilter.Add(".mov");
        picker.FileTypeFilter.Add(".webm");
        picker.FileTypeFilter.Add("*");

        var file = await picker.PickSingleFileAsync();
        if (file is null) return;

        MediaPlayer.Pause();
        if (MediaPlayer.Open(file.Path))
        {
            MediaPlayer.Play();
        }
    }
}
