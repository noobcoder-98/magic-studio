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

        Closed += (_, _) =>
        {
            //MediaPlayer.Dispose();
            FFplayPlayer.Dispose();
        };
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

        // Open the same file in both players simultaneously.
        //MediaPlayer.Pause();
        FFplayPlayer.Pause();

        //if (MediaPlayer.Open(file.Path))
        //    MediaPlayer.Play();

        if (FFplayPlayer.Open(file.Path))
            FFplayPlayer.Play();
    }
}
