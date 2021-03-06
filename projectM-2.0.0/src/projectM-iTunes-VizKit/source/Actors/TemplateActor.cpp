/*
 * Project: VizKit
 * Version: 1.9

 * Date: 20070503
 * File: TemplateActor.cpp
 *
 */

/***************************************************************************

Copyright (c) 2004-2007 Heiko Wichmann (http://www.imagomat.de/vizkit)


This software is provided 'as-is', without any expressed or implied warranty.
In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented;
   you must not claim that you wrote the original software.
   If you use this software in a product, an acknowledgment
   in the product documentation would be appreciated
   but is not required.

2. Altered source versions must be plainly marked as such,
   and must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.

 ***************************************************************************/

#include "TemplateActor.h"
#include "TemplateAction.h"
#include "VisualErrorHandling.h"

#include <string>



using namespace VizKit;


TemplateActor::TemplateActor()
{
    strcpy(actorName, "TEMPLATE");
    state = kVisActNoShow; // state must be kVisActOn to show the action of the actor as part of the VisualEnsemble (kVisActNoShow means not being called at all)
    theTemplateAction = new TemplateAction;
}


TemplateActor::~TemplateActor()
{
    delete theTemplateAction;
}


void TemplateActor::prepareShow(const VisualPlayerState& visualPlayerState)
{
    theTemplateAction->prepareTemplateAction();
}


void TemplateActor::show()
{
    theTemplateAction->show();
}


void TemplateActor::finishShow()
{
    theTemplateAction->finishTemplateAction();
}
