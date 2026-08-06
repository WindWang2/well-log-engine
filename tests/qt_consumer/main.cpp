#include <welllog/qtwidgets/well_log_view.hpp>

#include <QApplication>
#include <QVBoxLayout>
#include <QWidget>

int main(int argc, char **argv) {
  welllog::configure_well_log_surface_format();
  QApplication application(argc, argv);
  QWidget host;
  auto *layout = new QVBoxLayout(&host);
  auto *view = new welllog::WellLogView(&host);
  layout->addWidget(view);
  return view->parentWidget() == &host ? 0 : 1;
}
