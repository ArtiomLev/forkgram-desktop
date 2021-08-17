/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/linux_gtk_integration.h"

#ifdef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
#error "GTK integration depends on D-Bus integration."
#endif // DESKTOP_APP_DISABLE_DBUS_INTEGRATION

#include "base/platform/linux/base_linux_gtk_integration.h"
#include "base/platform/linux/base_linux_gtk_integration_p.h"
#include "base/platform/linux/base_linux_glibmm_helper.h"
#include "base/platform/linux/base_linux_dbus_utilities.h"
#include "base/platform/base_platform_info.h"
#include "platform/linux/linux_gtk_integration_p.h"
#include "platform/linux/linux_gdk_helper.h"
#include "platform/linux/linux_gtk_open_with_dialog.h"
#include "platform/linux/linux_wayland_integration.h"
#include "window/window_controller.h"
#include "core/application.h"

#ifndef DESKTOP_APP_DISABLE_WEBKITGTK
#include "webview/platform/linux/webview_linux_webkit2gtk.h"
#endif // !DESKTOP_APP_DISABLE_WEBKITGTK

#include <QtCore/QProcess>

#include <private/qguiapplication_p.h>
#include <giomm.h>

namespace Platform {
namespace internal {

using namespace Platform::Gtk;
using BaseGtkIntegration = base::Platform::GtkIntegration;

namespace {

constexpr auto kService = "org.telegram.desktop.GtkIntegration-%1"_cs;
constexpr auto kBaseService = "org.telegram.desktop.BaseGtkIntegration-%1"_cs;
constexpr auto kWebviewService = "org.telegram.desktop.GtkIntegration.WebviewHelper-%1-%2"_cs;
constexpr auto kObjectPath = "/org/telegram/desktop/GtkIntegration"_cs;
constexpr auto kInterface = "org.telegram.desktop.GtkIntegration"_cs;

constexpr auto kIntrospectionXML = R"INTROSPECTION(<node>
	<interface name='org.telegram.desktop.GtkIntegration'>
		<method name='Load'>
			<arg type='s' name='allowed-backends' direction='in'/>
		</method>
		<method name='ShowOpenWithDialog'>
			<arg type='s' name='parent' direction='in'/>
			<arg type='s' name='filepath' direction='in'/>
		</method>
		<signal name='OpenWithDialogResponse'>
			<arg type='b' name='result' direction='out'/>
		</signal>
	</interface>
</node>)INTROSPECTION"_cs;

Glib::ustring ServiceName;

} // namespace

class GtkIntegration::Private {
public:
	Private()
	: dbusConnection([] {
		try {
			return Gio::DBus::Connection::get_sync(
				Gio::DBus::BusType::BUS_TYPE_SESSION);
		} catch (...) {
			return Glib::RefPtr<Gio::DBus::Connection>();
		}
	}())
	, interfaceVTable(sigc::mem_fun(this, &Private::handleMethodCall)) {
	}

	void handleMethodCall(
		const Glib::RefPtr<Gio::DBus::Connection> &connection,
		const Glib::ustring &sender,
		const Glib::ustring &object_path,
		const Glib::ustring &interface_name,
		const Glib::ustring &method_name,
		const Glib::VariantContainerBase &parameters,
		const Glib::RefPtr<Gio::DBus::MethodInvocation> &invocation);

	const Glib::RefPtr<Gio::DBus::Connection> dbusConnection;
	const Gio::DBus::InterfaceVTable interfaceVTable;
	Glib::RefPtr<Gio::DBus::NodeInfo> introspectionData;
	Glib::ustring parentDBusName;
	bool remoting = true;
	uint registerId = 0;
	uint parentServiceWatcherId = 0;
};

void GtkIntegration::Private::handleMethodCall(
		const Glib::RefPtr<Gio::DBus::Connection> &connection,
		const Glib::ustring &sender,
		const Glib::ustring &object_path,
		const Glib::ustring &interface_name,
		const Glib::ustring &method_name,
		const Glib::VariantContainerBase &parameters,
		const Glib::RefPtr<Gio::DBus::MethodInvocation> &invocation) {
	if (sender != parentDBusName) {
		Gio::DBus::Error error(
			Gio::DBus::Error::ACCESS_DENIED,
			"Access denied.");

		invocation->return_error(error);
		return;
	}

	try {
		const auto integration = Instance();
		if (!integration) {
			throw std::exception();
		}

		auto parametersCopy = parameters;

		if (method_name == "Load") {
			const auto allowedBackends = base::Platform::GlibVariantCast<
				Glib::ustring>(parametersCopy.get_child(0));

			integration->load(QString::fromStdString(allowedBackends));
			invocation->return_value({});
			return;
		} else if (method_name == "ShowOpenWithDialog") {
			const auto parent = base::Platform::GlibVariantCast<
				Glib::ustring>(parametersCopy.get_child(0));

			const auto filepath = base::Platform::GlibVariantCast<
				Glib::ustring>(parametersCopy.get_child(1));

			const auto dialog = File::internal::CreateGtkOpenWithDialog(
				QString::fromStdString(parent),
				QString::fromStdString(filepath)).release();

			if (dialog) {
				dialog->response(
				) | rpl::start_with_next([=](bool response) {
					try {
						connection->emit_signal(
							std::string(kObjectPath),
							std::string(kInterface),
							"OpenWithDialogResponse",
							parentDBusName,
							base::Platform::MakeGlibVariant(std::tuple{
								response,
							}));
					} catch (...) {
					}

					delete dialog;
				}, dialog->lifetime());

				invocation->return_value({});
				return;
			}
		}
	} catch (...) {
	}

	Gio::DBus::Error error(
		Gio::DBus::Error::UNKNOWN_METHOD,
		"Method does not exist.");

	invocation->return_error(error);
}

GtkIntegration::GtkIntegration()
: _private(std::make_unique<Private>()) {
}

GtkIntegration::~GtkIntegration() {
	if (_private->dbusConnection) {
		if (_private->parentServiceWatcherId != 0) {
			_private->dbusConnection->signal_unsubscribe(
				_private->parentServiceWatcherId);
		}

		if (_private->registerId != 0) {
			_private->dbusConnection->unregister_object(
				_private->registerId);
		}
	}
}

GtkIntegration *GtkIntegration::Instance() {
	if (!BaseGtkIntegration::Instance()) {
		return nullptr;
	}

	static GtkIntegration instance;
	return &instance;
}

void GtkIntegration::load(const QString &allowedBackends) {
	static bool Loaded = false;
	Expects(!Loaded);

	if (_private->remoting) {
		if (!_private->dbusConnection) {
			return;
		}

		try {
			auto reply = _private->dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"Load",
				base::Platform::MakeGlibVariant(std::tuple{
					Glib::ustring(allowedBackends.toStdString()),
				}),
				ServiceName);
		} catch (...) {
		}

		return;
	}

	BaseGtkIntegration::Instance()->load(allowedBackends, true);
	if (!BaseGtkIntegration::Instance()->loaded()) {
		return;
	}

	auto &lib = BaseGtkIntegration::Instance()->library();

	LOAD_GTK_SYMBOL(lib, gtk_widget_show);
	LOAD_GTK_SYMBOL(lib, gtk_widget_get_window);
	LOAD_GTK_SYMBOL(lib, gtk_widget_realize);
	LOAD_GTK_SYMBOL(lib, gtk_widget_destroy);

	LOAD_GTK_SYMBOL(lib, gtk_app_chooser_dialog_new);
	LOAD_GTK_SYMBOL(lib, gtk_app_chooser_get_app_info);
	LOAD_GTK_SYMBOL(lib, gtk_app_chooser_get_type);

	GdkHelperLoad(lib);
	Loaded = true;
}

int GtkIntegration::exec(const QString &parentDBusName) {
	_private->remoting = false;
	_private->parentDBusName = parentDBusName.toStdString();

	_private->introspectionData = Gio::DBus::NodeInfo::create_for_xml(
		std::string(kIntrospectionXML));

	_private->registerId = _private->dbusConnection->register_object(
		std::string(kObjectPath),
		_private->introspectionData->lookup_interface(),
		_private->interfaceVTable);

	const auto app = Gio::Application::create(ServiceName);
	app->hold();
	_private->parentServiceWatcherId = base::Platform::DBus::RegisterServiceWatcher(
		_private->dbusConnection,
		parentDBusName.toStdString(),
		[=](
			const Glib::ustring &service,
			const Glib::ustring &oldOwner,
			const Glib::ustring &newOwner) {
			if (!newOwner.empty()) {
				return;
			}
			app->quit();
		});
	return app->run(0, nullptr);
}

bool GtkIntegration::showOpenWithDialog(const QString &filepath) const {
	const auto parent = [&] {
		if (const auto activeWindow = Core::App().activeWindow()) {
			if (const auto integration = WaylandIntegration::Instance()) {
				if (const auto handle = integration->nativeHandle(
					activeWindow->widget()->windowHandle())
					; !handle.isEmpty()) {
					return qsl("wayland:") + handle;
				}
			} else if (Platform::IsX11()) {
				return qsl("x11:") + QString::number(
					activeWindow->widget()->winId(),
					16);
			}
		}
		return QString();
	}();

	if (_private->remoting) {
		if (!_private->dbusConnection) {
			return false;
		}

		try {
			_private->dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"ShowOpenWithDialog",
				base::Platform::MakeGlibVariant(std::tuple{
					Glib::ustring(parent.toStdString()),
					Glib::ustring(filepath.toStdString()),
				}),
				ServiceName);

			const auto context = Glib::MainContext::create();
			const auto loop = Glib::MainLoop::create(context);
			g_main_context_push_thread_default(context->gobj());
			const auto contextGuard = gsl::finally([&] {
				g_main_context_pop_thread_default(context->gobj());
			});
			bool result = false;

			const auto signalId = _private->dbusConnection->signal_subscribe(
				[&](
					const Glib::RefPtr<Gio::DBus::Connection> &connection,
					const Glib::ustring &sender_name,
					const Glib::ustring &object_path,
					const Glib::ustring &interface_name,
					const Glib::ustring &signal_name,
					Glib::VariantContainerBase parameters) {
					try {
						auto parametersCopy = parameters;

						result = base::Platform::GlibVariantCast<bool>(
								parametersCopy.get_child(0));

						loop->quit();
					} catch (...) {
					}
				},
				ServiceName,
				std::string(kInterface),
				"OpenWithDialogResponse",
				std::string(kObjectPath));

			const auto signalGuard = gsl::finally([&] {
				if (signalId != 0) {
					_private->dbusConnection->signal_unsubscribe(signalId);
				}
			});

			if (signalId != 0) {
				QWindow window;
				QGuiApplicationPrivate::showModalWindow(&window);
				loop->run();
				QGuiApplicationPrivate::hideModalWindow(&window);
			}

			return result;
		} catch (...) {
		}

		return false;
	}

	const auto dialog = File::internal::CreateGtkOpenWithDialog(
		parent,
		filepath);

	if (!dialog) {
		return false;
	}

	const auto context = Glib::MainContext::create();
	const auto loop = Glib::MainLoop::create(context);
	g_main_context_push_thread_default(context->gobj());
	bool result = false;

	dialog->response(
	) | rpl::start_with_next([&](bool response) {
		result = response;
		loop->quit();
	}, dialog->lifetime());

	QWindow window;
	QGuiApplicationPrivate::showModalWindow(&window);
	loop->run();
	g_main_context_pop_thread_default(context->gobj());
	QGuiApplicationPrivate::hideModalWindow(&window);

	return result;
}

QString GtkIntegration::AllowedBackends() {
	return Platform::IsWayland()
		? qsl("wayland,x11")
		: Platform::IsX11()
			? qsl("x11,wayland")
			: QString();
}

int GtkIntegration::Exec(
		Type type,
		const QString &parentDBusName,
		const QString &serviceName) {
	Glib::init();
	Gio::init();

	if (type == Type::Base) {
		BaseGtkIntegration::SetServiceName(serviceName);
		if (const auto integration = BaseGtkIntegration::Instance()) {
			return integration->exec(parentDBusName);
		}
#ifndef DESKTOP_APP_DISABLE_WEBKITGTK
	} else if (type == Type::Webview) {
		Webview::WebKit2Gtk::SetServiceName(serviceName.toStdString());
		return Webview::WebKit2Gtk::Exec(parentDBusName.toStdString());
#endif // !DESKTOP_APP_DISABLE_WEBKITGTK
	} else if (type == Type::TDesktop) {
		ServiceName = serviceName.toStdString();
		if (const auto integration = Instance()) {
			return integration->exec(parentDBusName);
		}
	}

	return 1;
}

void GtkIntegration::Start(Type type) {
	if (type != Type::Base
		&& type != Type::Webview
		&& type != Type::TDesktop) {
		return;
	}

	const auto d = QFile::encodeName(QDir(cWorkingDir()).absolutePath());
	char h[33] = { 0 };
	hashMd5Hex(d.constData(), d.size(), h);

	if (type == Type::Base) {
		BaseGtkIntegration::SetServiceName(kBaseService.utf16().arg(h));
	} else if (type == Type::Webview) {
#ifndef DESKTOP_APP_DISABLE_WEBKITGTK
		Webview::WebKit2Gtk::SetServiceName(
			kWebviewService.utf16().arg(h).arg("%1").toStdString());
#endif // !DESKTOP_APP_DISABLE_WEBKITGTK

		return;
	} else {
		ServiceName = kService.utf16().arg(h).toStdString();
	}

	const auto dbusName = [] {
		try {
			static const auto connection = Gio::DBus::Connection::get_sync(
				Gio::DBus::BusType::BUS_TYPE_SESSION);

			return QString::fromStdString(connection->get_unique_name());
		} catch (...) {
			return QString();
		}
	}();

	if (dbusName.isEmpty()) {
		return;
	}

	QProcess::startDetached(cExeDir() + cExeName(), {
		(type == Type::Base)
			? qsl("-basegtkintegration")
			: qsl("-gtkintegration"),
		dbusName,
		(type == Type::Base)
			? kBaseService.utf16().arg(h)
			: kService.utf16().arg(h),
	});
}

void GtkIntegration::Autorestart(Type type) {
	if (type != Type::Base && type != Type::TDesktop) {
		return;
	}

	try {
		static const auto connection = Gio::DBus::Connection::get_sync(
			Gio::DBus::BusType::BUS_TYPE_SESSION);

		base::Platform::DBus::RegisterServiceWatcher(
			connection,
			(type == Type::Base)
				? Glib::ustring(BaseGtkIntegration::ServiceName().toStdString())
				: ServiceName,
			[=](
				const Glib::ustring &service,
				const Glib::ustring &oldOwner,
				const Glib::ustring &newOwner) {
				if (newOwner.empty()) {
					Start(type);
				} else {
					if (type == Type::Base) {
						if (const auto integration = BaseGtkIntegration::Instance()) {
							integration->load(AllowedBackends());
						}
					} else if (type == Type::TDesktop) {
						if (const auto integration = Instance()) {
							integration->load(AllowedBackends());
						}
					}
				}
			});
	} catch (...) {
	}
}

} // namespace internal
} // namespace Platform
