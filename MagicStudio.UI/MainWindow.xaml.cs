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

        QuadPlayer.OpenRequested += OnOpenRequested;

        Closed += (_, _) => QuadPlayer.Dispose();
    }

    private async void OnOpenRequested(object? sender,
        QuadFFplayControl.SlotOpenRequestedEventArgs e)
    {
        var path = await PickVideoAsync();
        if (path is null) return;
        QuadPlayer.OpenSlot(e.Index, path);
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
}
