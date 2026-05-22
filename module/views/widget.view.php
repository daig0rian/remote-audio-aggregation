<?php declare(strict_types = 0);

/**
 * RAA Monitor widget view.
 *
 * @var CView  $this
 * @var array  $data
 */

$view = new CWidgetView($data);

$body = (new CDiv(
	(new CDiv([
		(new CDiv([
			(new CButton('', _('▶  Monitor')))
				->addClass('raa-play-btn'),
			(new CDiv(_('Idle')))
				->addClass('raa-status'),
		]))->addClass('raa-row-top'),
		(new CDiv(
			(new CDiv())->addClass('raa-bar')
		))->addClass('raa-meter'),
		(new CLink(_('Mixer Settings'), '#'))
			->addClass('raa-admin-link')
			->setAttribute('target', '_blank'),
	]))->addClass('raa-controls')
))->addClass('raa-player');

$view
	->addItem($body)
	->setVar('fields', $data['fields'])
	->show();
