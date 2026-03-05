function Component() {}

Component.prototype.createOperations = function()
{
    component.createOperations();
    var version = component.value("Version");

    if (systemInfo.productType === "windows") {
        // Start Menu shortcut (always)
        component.addOperation("CreateShortcut",
            "@TargetDir@/" + version + "/MinervaStation.exe",
            "@StartMenuDir@/MinervaStation " + version + ".lnk",
            "workingDirectory=@TargetDir@/" + version,
            "iconPath=@TargetDir@/" + version + "/MinervaStation.exe");

        // Desktop shortcut (opt-in)
        var page = gui.pageWidgetByObjectName("DynamicShortcutPage");
        if (page) {
            var checkbox = page.findChild("desktopShortcutCheckBox");
            if (checkbox && checkbox.checked) {
                component.addOperation("CreateShortcut",
                    "@TargetDir@/" + version + "/MinervaStation.exe",
                    "@DesktopDir@/MinervaStation.lnk",
                    "workingDirectory=@TargetDir@/" + version,
                    "iconPath=@TargetDir@/" + version + "/MinervaStation.exe");
            }
        }
    }
}

Component.prototype.loaded = function()
{
    if (installer.isInstaller()) {
        installer.addWizardPageItem(component, "ShortcutPage", QInstaller.TargetDirectory);
    }
}
