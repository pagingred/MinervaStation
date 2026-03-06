function Controller() {
    installer.setDefaultPageVisible(QInstaller.Introduction, true);
}

Controller.prototype.IntroductionPageCallback = function() {
    var page = gui.currentPageWidget();
    if (page) {
        page.title = "";
        page.subTitle = "";
    }
}
