@{
    Id                 = 'Record'
    Name               = 'Sound Recorder'
    RootScript         = 'Record.ps1'
    EntryPoint         = 'Start-Record'
    Style              = 'Record.Style.psd1'
    Abi                 = 'WASAPI.abi.h'
    RequiresDirectPort = $true

    DefaultWidth       = 460
    DefaultHeight      = 420
    MinimumWidth       = 360
    MinimumHeight      = 360
}
