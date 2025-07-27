#include <QApplication>
#include <QPushButton>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QPushButton button("Hello, Qt6 on Wayland!");
    button.resize(300, 100);
    button.showFullScreen();
    return app.exec();
}
