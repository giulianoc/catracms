package com.catramms.backing.workflowEditor.Properties;

import com.catramms.backing.workflowEditor.utility.WorkflowIssue;
import com.catramms.backing.workflowEditor.utility.IngestionData;
import org.apache.log4j.Logger;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.Serializable;

public class FrameProperties extends CreateContentProperties implements Serializable {

    private static final Logger mLogger = Logger.getLogger(FrameProperties.class);

    private Long timeInSecondsDecimalsPrecision;
    private Float instantInSeconds;
    private Long width;
    private Long height;

    private StringBuilder taskReferences = new StringBuilder();

    public FrameProperties(String positionX, String positionY,
                           int elementId, String label)
    {
        super(positionX, positionY, elementId, label, "Frame" + "-icon.png", "Task", "Frame");

        timeInSecondsDecimalsPrecision = new Long(6);
    }

    public FrameProperties clone()
    {
        FrameProperties frameProperties = new FrameProperties(
                super.getPositionX(), super.getPositionY(),
                super.getElementId(), super.getLabel());

        frameProperties.setInstantInSeconds(instantInSeconds);
        frameProperties.setWidth(width);
        frameProperties.setHeight(height);

        frameProperties.setStringBuilderTaskReferences(taskReferences);

        super.getData(frameProperties);


        return frameProperties;
    }

    public void setData(FrameProperties workflowProperties)
    {
        super.setData(workflowProperties);

        setInstantInSeconds(workflowProperties.getInstantInSeconds());
        setWidth(workflowProperties.getWidth());
        setHeight(workflowProperties.getHeight());

        setStringBuilderTaskReferences(workflowProperties.getStringBuilderTaskReferences());
    }

    public void setData(JSONObject jsonWorkflowElement)
    {
        try {
            super.setData(jsonWorkflowElement);

            setLabel(jsonWorkflowElement.getString("Label"));

            JSONObject joParameters = jsonWorkflowElement.getJSONObject("Parameters");

            if (joParameters.has("InstantInSeconds"))
                setInstantInSeconds(new Float(joParameters.getDouble("InstantInSeconds")));
            if (joParameters.has("Width"))
                setWidth(joParameters.getLong("Width"));
            if (joParameters.has("Height"))
                setHeight(joParameters.getLong("Height"));

            if (joParameters.has("References"))
            {
                String references = "";
                JSONArray jaReferences = joParameters.getJSONArray("References");
                for (int referenceIndex = 0; referenceIndex < jaReferences.length(); referenceIndex++)
                {
                    JSONObject joReference = jaReferences.getJSONObject(referenceIndex);

                    if (joReference.has("ReferencePhysicalPathKey"))
                    {
                        if (references.equalsIgnoreCase(""))
                            references = new Long(joReference.getLong("ReferencePhysicalPathKey")).toString();
                        else
                            references += ("," + new Long(joReference.getLong("ReferencePhysicalPathKey")).toString());
                    }
                }

                setTaskReferences(references);
            }
        }
        catch (Exception e)
        {
            mLogger.error("WorkflowProperties:setData failed, exception: " + e);
        }
    }

    public JSONObject buildWorkflowElementJson(IngestionData ingestionData)
            throws Exception
    {
        JSONObject jsonWorkflowElement = new JSONObject();

        try
        {
            jsonWorkflowElement.put("Type", super.getType());

            JSONObject joParameters = new JSONObject();
            jsonWorkflowElement.put("Parameters", joParameters);

            mLogger.info("task.getType: " + super.getType());

            if (super.getLabel() != null && !super.getLabel().equalsIgnoreCase(""))
                jsonWorkflowElement.put("Label", super.getLabel());
            else
            {
                WorkflowIssue workflowIssue = new WorkflowIssue();
                workflowIssue.setLabel("");
                workflowIssue.setFieldName("Label");
                workflowIssue.setTaskType(super.getType());
                workflowIssue.setIssue("The field is not initialized");

                ingestionData.getWorkflowIssueList().add(workflowIssue);
            }

            if (getInstantInSeconds() != null)
                joParameters.put("InstantInSeconds", getInstantInSeconds());
            if (getWidth() != null)
                joParameters.put("Width", getWidth());
            if (getHeight() != null)
                joParameters.put("Height", getHeight());

            super.addCreateContentPropertiesToJson(joParameters);

            if (taskReferences != null && !taskReferences.toString().equalsIgnoreCase(""))
            {
                JSONArray jaReferences = new JSONArray();
                joParameters.put("References", jaReferences);

                String [] physicalPathKeyReferences = taskReferences.toString().split(",");
                for (String physicalPathKeyReference: physicalPathKeyReferences)
                {
                    JSONObject joReference = new JSONObject();
                    joReference.put("ReferencePhysicalPathKey", Long.parseLong(physicalPathKeyReference.trim()));

                    jaReferences.put(joReference);
                }
            }

            super.addEventsPropertiesToJson(jsonWorkflowElement, ingestionData);
        }
        catch (Exception e)
        {
            mLogger.error("buildWorkflowJson failed: " + e);

            throw e;
        }

        return jsonWorkflowElement;
    }

    public Long getTimeInSecondsDecimalsPrecision() {
        return timeInSecondsDecimalsPrecision;
    }

    public void setTimeInSecondsDecimalsPrecision(Long timeInSecondsDecimalsPrecision) {
        this.timeInSecondsDecimalsPrecision = timeInSecondsDecimalsPrecision;
    }

    public Float getInstantInSeconds() {
        return instantInSeconds;
    }

    public void setInstantInSeconds(Float instantInSeconds) {
        this.instantInSeconds = instantInSeconds;
    }

    public Long getWidth() {
        return width;
    }

    public void setWidth(Long width) {
        this.width = width;
    }

    public Long getHeight() {
        return height;
    }

    public void setHeight(Long height) {
        this.height = height;
    }

    public void setStringBuilderTaskReferences(StringBuilder taskReferences) {
        this.taskReferences = taskReferences;
    }

    public StringBuilder getStringBuilderTaskReferences() {
        return taskReferences;
    }

    public String getTaskReferences() {
        return taskReferences.toString();
    }

    public void setTaskReferences(String taskReferences) {
        this.taskReferences.replace(0, this.taskReferences.length(), taskReferences);
    }
}
