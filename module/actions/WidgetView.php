<?php declare(strict_types = 0);

namespace Modules\RaaMonitor\Actions;

use CControllerDashboardWidgetView,
	CControllerResponseData;

class WidgetView extends CControllerDashboardWidgetView {

	protected function doAction(): void {
		$this->setResponse(new CControllerResponseData([
			'name'   => $this->getInput('name', $this->widget->getDefaultName()),
			'fields' => [
				'server_host' => $this->fields_values['server_host'],
				'ws_port'     => (int) $this->fields_values['ws_port'],
				'buffer_ms'   => (int) $this->fields_values['buffer_ms'],
			],
			'user' => [
				'debug_mode' => $this->getDebugMode()
			]
		]));
	}
}
