#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QTimer>

namespace {

// The monitor always uses a dark theme (video-oriented tool), independent of the desktop's theme.
void applyDarkTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48);
    const QColor base(30, 30, 32);
    const QColor text(225, 225, 225);
    const QColor disabled(120, 120, 120);
    const QColor accent(42, 130, 218);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(55, 55, 58));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Mid, QColor(70, 70, 74));
    p.setColor(QPalette::Dark, QColor(25, 25, 27));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    app.setPalette(p);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Qt 6.10's PipeWire audio backend does not list Bluetooth outputs and reports the wrong default device;
    // the PulseAudio backend (served by pipewire-pulse) follows the system default correctly.
    if (qEnvironmentVariableIsEmpty("QT_AUDIO_BACKEND"))
        qputenv("QT_AUDIO_BACKEND", "pulseaudio");

    QApplication app(argc, argv);
    QApplication::setOrganizationName("arib-caption-monitor");
    QApplication::setApplicationName("arib-caption-monitor");
    QApplication::setApplicationVersion(APP_VERSION);
    QGuiApplication::setDesktopFileName(APP_ID);  // window icon/grouping on Wayland
    applyDarkTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription("Monitor de legendas ARIB (ABNT NBR 15606-1) recebidas por MPEG-TS/UDP");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("arquivo", "Arquivo MPEG-TS a reproduzir (igual a --file).", "[arquivo.ts]");
    QCommandLineOption portOption({"p", "port"}, "Porta UDP de recepção.", "porta");
    QCommandLineOption groupOption({"m", "multicast"}, "Grupo multicast (opcional).", "endereço");
    QCommandLineOption snapshotOption("snapshot", "Salva uma captura da janela e encerra (teste).", "arquivo.png");
    QCommandLineOption snapshotAfterOption("snapshot-after", "Segundos até a captura (padrão 10).", "segundos", "10");
    QCommandLineOption fileOption({"f", "file"}, "Reproduz um arquivo MPEG-TS em vez de escutar UDP.", "arquivo.ts");
    QCommandLineOption tabOption("tab", "Aba do painel ao abrir (0 MPEG-TS, 1 Validação, 2 Comandos, 3 Log).", "índice");
    parser.addOptions({portOption, groupOption, snapshotOption, snapshotAfterOption, tabOption, fileOption});
    parser.process(app);

    MainWindow window;
    QString error;
    if (!window.initialize(&error)) {
        QMessageBox::critical(nullptr, "ARIB Caption Monitor", error);
        return 1;
    }
    if (parser.isSet(portOption))
        window.setEndpoint(static_cast<quint16>(parser.value(portOption).toUInt()), parser.value(groupOption));
    if (parser.isSet(tabOption))
        window.showAnalysisTab(parser.value(tabOption).toInt());
    window.show();
    const QString file = parser.isSet(fileOption) ? parser.value(fileOption) : parser.positionalArguments().value(0);
    // The app opens idle and the user chooses UDP or a file in the window. A file passed by the desktop
    // ("Open with") or an explicit --port (used by tests) starts right away.
    if (!file.isEmpty())
        window.openFile(file);
    else if (parser.isSet(portOption))
        window.startReceiving();

    if (parser.isSet(snapshotOption)) {
        const QString path = parser.value(snapshotOption);
        QTimer::singleShot(parser.value(snapshotAfterOption).toInt() * 1000, &window, [&window, path]() {
            const QPixmap shot = window.grab();
            if (!shot.save(path))
                qWarning("snapshot: falha ao salvar %s (%dx%d)", qPrintable(path), shot.width(), shot.height());
            QApplication::quit();
        });
    }

    return app.exec();
}
