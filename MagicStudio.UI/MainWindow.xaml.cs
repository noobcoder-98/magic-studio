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
            FFplayPlayer.Dispose();
            DualPlayer.Dispose();
        };
    }

    private async System.Threading.Tasks.Task<string?> PickVideoAsync()
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
        return file?.Path;
    }

    private async void OpenFile_Click(object sender, RoutedEventArgs e)
    {
        var path = await PickVideoAsync();
        if (path is null) return;

        FFplayPlayer.Pause();
        if (FFplayPlayer.Open(path))
            FFplayPlayer.Play();
    }

    private async void OpenDualA_Click(object sender, RoutedEventArgs e)
    {
        var path = await PickVideoAsync();
        if (path is not null) DualPlayer.OpenA(path);
    }

    private async void OpenDualB_Click(object sender, RoutedEventArgs e)
    {
        var path = await PickVideoAsync();
        if (path is not null) DualPlayer.OpenB(path);
    }
}
