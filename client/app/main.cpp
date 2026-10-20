// cloud-media 客户端入口。
// P8-1：真实登录 + 文件列表。网络在独立线程（netapi），UI 只收信号。
//
// 用法：
//   cloud-media-client --version
//   cloud-media-client --host 192.168.142.131 --port 8004 --tel 139... --password-env CM_CLI_PASSWORD
//   cloud-media-client --host ... --smoke            # 无界面：登录+列表，打印结果后退出（CI/联调取证）
//   cloud-media-client --ui-demo [--screenshot out.png]
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "cm/protocol/version.h"
#include "cm/version.h"
#include "netapi/NetClient.h"
#include "ui/LoginDialog.h"
#include "ui/MainWindow.h"

namespace {

struct Options {
    QString host;
    quint16 port;
    QString tel;
    QString password;
    bool demo;
    bool smoke;
    QString screenshot;

    Options() : host(QString::fromUtf8("127.0.0.1")), port(8004), demo(false),
                smoke(false) {}
};

// 深色主题基线：色板与字号见 docs/specs/08-界面设计契约.md §4。
void applyTheme(QApplication* app) {
    QFont font(QString::fromUtf8("Microsoft YaHei UI"), 9);
    app->setFont(font);
    QFile qss(QString::fromUtf8(":/app.qss"));
    if (qss.open(QIODevice::ReadOnly)) {
        app->setStyleSheet(QString::fromUtf8(qss.readAll()));
    }
}

const char* utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return bytes.constData();
}

// 无界面冒烟：登录 → 列表 → 打印 → 退出。退出码 0 成功 / 2 登录被拒 / 3 超时 / 4 列表失败。
int runSmoke(cm::netapi::NetClient* net, const Options& opt) {
    QObject::connect(net, &cm::netapi::NetClient::linkState,
                     [](int state, const QString& detail) {
        std::printf("LINK state=%d detail=%s\n", state, utf8(detail));
        std::fflush(stdout);
    });
    QObject::connect(net, &cm::netapi::NetClient::loginFinished,
                     [net](quint32 code, quint64 uid, const QString& name) {
        if (code != 0) {
            std::printf("LOGIN_FAIL code=%u\n", code);
            std::fflush(stdout);
            QCoreApplication::exit(2);
            return;
        }
        std::printf("LOGIN_OK uid=%llu name=%s\n",
                    static_cast<unsigned long long>(uid), utf8(name));
        std::fflush(stdout);
        net->listFiles(0, 50);
    });
    QObject::connect(net, &cm::netapi::NetClient::listFinished,
                     [](quint32 code, const QVariantList& rows) {
        std::printf("LIST code=%u rows=%d\n", code, rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows.at(i).toMap();
            std::printf("ROW %s size=%llu dir=%d\n",
                        utf8(row.value(QString::fromUtf8("name")).toString()),
                        static_cast<unsigned long long>(
                            row.value(QString::fromUtf8("size")).toULongLong()),
                        row.value(QString::fromUtf8("isDir")).toBool() ? 1 : 0);
        }
        std::fflush(stdout);
        std::printf("SMOKE_OK\n");
        std::fflush(stdout);
        QCoreApplication::exit(code == 0 ? 0 : 4);
    });
    QObject::connect(net, &cm::netapi::NetClient::requestFailed,
                     [](quint16 type, const QString& reason) {
        std::printf("REQ_FAIL type=%u reason=%s\n", type, utf8(reason));
        std::fflush(stdout);
    });
    QTimer::singleShot(15000, []() {
        std::printf("SMOKE_TIMEOUT\n");
        std::fflush(stdout);
        QCoreApplication::exit(3);
    });
    net->login(opt.tel, opt.password);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--version") {
            std::cout << "cloud-media-client " << cm::version()
                      << " (protocol v" << cm::protocol::version() << ")\n";
            return 0;
        }
        if (arg == "--host" && i + 1 < argc) {
            opt.host = QString::fromStdString(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            opt.port = static_cast<quint16>(std::atoi(argv[++i]));
        } else if (arg == "--tel" && i + 1 < argc) {
            opt.tel = QString::fromStdString(argv[++i]);
        } else if (arg == "--password" && i + 1 < argc) {
            opt.password = QString::fromStdString(argv[++i]);
        } else if (arg == "--password-env" && i + 1 < argc) {
            const char* value = std::getenv(argv[++i]);
            if (value != nullptr) {
                opt.password = QString::fromUtf8(value);
            }
        } else if (arg == "--ui-demo") {
            opt.demo = true;
        } else if (arg == "--smoke") {
            opt.smoke = true;
        } else if (arg == "--screenshot" && i + 1 < argc) {
            opt.screenshot = QString::fromStdString(argv[++i]);
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8("cloud-media"));
    applyTheme(&app);

    cm::netapi::NetClient net;
    net.connectTo(opt.host, opt.port);

    if (opt.smoke) {
        if (opt.tel.isEmpty() || opt.password.isEmpty()) {
            std::printf("SMOKE_NEED_CREDENTIALS\n");
            return 1;
        }
        runSmoke(&net, opt);
        return app.exec();
    }

    if (opt.demo) {
        cm::ui::MainWindow window(&net);
        window.show();
        if (!opt.screenshot.isEmpty()) {
            const QString path = opt.screenshot;
            QTimer::singleShot(600, [&window, path]() {
                window.grab().save(path);
                QApplication::quit();
            });
        }
        return app.exec();
    }

    const bool autoLogin = !opt.tel.isEmpty() && !opt.password.isEmpty();
    if (!autoLogin) {
        cm::ui::LoginDialog login(&net);
        if (login.exec() != QDialog::Accepted) {
            return 0;
        }
        cm::ui::MainWindow window(&net);
        window.show();
        return app.exec();
    }

    cm::ui::MainWindow window(&net);
    window.show();
    net.login(opt.tel, opt.password);
    if (!opt.screenshot.isEmpty()) {
        const QString path = opt.screenshot;
        // 等登录 + 列表回来再截（本地回环通常 <200ms，这里给 2.5s 余量）。
        QTimer::singleShot(2500, [&window, path]() {
            window.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
